#!/usr/bin/env python3
"""Generate Sniper's RISC-V decoder and core-model tables from the RISC-V
Unified Database.

    gen_isa.py --udb <riscv-unified-db> [--udb <overlay> ...] \
               --policy tools/riscv/isa_policy.yaml --out decoder_lib [--report]

Every --udb root is a UDB checkout or an overlay laid out the same way
(spec/std/isa/inst/<Ext>/<insn>.yaml); later roots override instructions of
the same name, so an extension under review (IME 0.9) drops in on top of the
ratified ISA.  The policy file selects extensions, classes instructions for
the core model and gives each class a placeholder latency.  Outputs:

  decoder_lib/riscv_isa_gen.h    class enum, flags, table sizes
  decoder_lib/riscv_isa_gen.cc   instruction table, dispatch buckets, class table,
                                 and the expected decode of every sample word
  (stdout with --report)         membership per class, conflicts, warnings

The generated files are checked in; rerun this after changing the policy or
dropping in a new specification.  decoder_lib/riscv_isa.{h,cc} (hand written)
is the API over the tables."""

import argparse, glob, os, re, sys
import yaml

FIELD_KINDS = ['XREG', 'FREG', 'VREG', 'IMM', 'CSR', 'OTHER']
FLAG_BITS = ['load', 'store', 'branch', 'jump', 'indirect', 'atomic', 'fp', 'vector',
             'footprint', 'busy', 'matrix', 'serializing', 'prefetch', 'cache_flush']
CPI_GROUPS = ['generic', 'branch', 'fp_addsub', 'fp_muldiv', 'ldst']
MAX_FIELDS = 7
MAX_SEGS = 8
# Older UDB drafts (the IME overlay among them) name register fields rs1/rd;
# the schema now says xs1/xd.  Both are accepted.
RENAME = {'rs1': 'xs1', 'rs2': 'xs2', 'rd': 'xd', 'rs3': 'xs3',
          'frs1': 'fs1', 'frs2': 'fs2', 'frs3': 'fs3', 'frd': 'fd'}
REG_KIND = {'x': 'XREG', 'f': 'FREG', 'v': 'VREG'}


def load_insts(roots):
    insts = {}
    for root in roots:
        base = os.path.join(root, 'spec', 'std', 'isa', 'inst')
        files = sorted(glob.glob(os.path.join(base, '**', '*.yaml'), recursive=True))
        if not files:
            sys.exit('%s: no instruction YAML under spec/std/isa/inst' % root)
        for f in files:
            d = yaml.safe_load(open(f))
            if not isinstance(d, dict) or d.get('kind') != 'instruction':
                continue
            d['_file'] = os.path.relpath(f, root)
            insts[d['name']] = d          # a later root overrides
    return insts


def extensions_of(defined_by):
    """Every extension name mentioned in a definedBy tree."""
    out = []
    def walk(n):
        if isinstance(n, dict):
            if 'name' in n and isinstance(n['name'], str) and set(n.keys()) <= {'name', 'version'}:
                out.append(n['name'])
            for v in n.values():
                walk(v)
        elif isinstance(n, list):
            for v in n:
                walk(v)
    walk(defined_by)
    return out


def defined_for_xlen(defined_by, xlen):
    """Evaluate a definedBy tree with every extension present and the given xlen."""
    def ev(n):
        if isinstance(n, dict):
            if 'xlen' in n and len(n) == 1:
                return n['xlen'] == xlen
            if 'extension' in n:
                return ev(n['extension'])
            if 'name' in n and 'xlen' not in n:
                return True
            if 'anyOf' in n:
                return any(ev(x) for x in n['anyOf'])
            if 'allOf' in n:
                return all(ev(x) for x in n['allOf'])
            if 'oneOf' in n:
                return any(ev(x) for x in n['oneOf'])
            if 'not' in n:
                return not ev(n['not'])
            return all(ev(v) for v in n.values())
        if isinstance(n, list):
            return all(ev(x) for x in n)
        return True
    return ev(defined_by)


def parse_location(loc):
    """'31|7|30-25|11-8' -> [(31,31),(7,7),(30,25),(11,8)], most significant first."""
    segs = []
    for part in str(loc).split('|'):
        if '-' in part:
            a, b = part.split('-')
            segs.append((int(a), int(b)))
        else:
            segs.append((int(part), int(part)))
    return segs


def field_of(var, length, name):
    fname = RENAME.get(var['name'], var['name'])
    segs = parse_location(var['location'])
    width = sum(m - l + 1 for m, l in segs)
    m = re.match(r'^([xfv])(d|s[123])$', fname)
    kind = REG_KIND[m.group(1)] if m else ('CSR' if fname == 'csr' else 'IMM' if fname in ('imm', 'uimm', 'shamt', 'cimm', 'spimm', 'offset', 'index') else 'OTHER')
    flags = []
    if m:
        # 3-bit register fields (compressed encodings) name x8..x15 / f8..f15
        if width == 3:
            flags.append('PLUS8')
        if m.group(2) == 'd':
            flags.append('WRITE')
        else:
            flags.append('READ')
    if var.get('sign_extend'):
        flags.append('SEXT')
    return dict(name=fname, kind=kind, segs=segs, shift=int(var.get('left_shift', 0)), flags=flags, width=width)


def classify(policy, name, exts, asm):
    for rule in policy['classes']:
        if 'name' in rule and not re.search(rule['name'], name):
            continue
        if 'ext' in rule and not any(re.search(rule['ext'], e) for e in exts):
            continue
        if 'asm' in rule and not re.search(rule['asm'], asm):
            continue
        return rule
    sys.exit('no class rule matched %s' % name)


def mem_size(policy, name):
    for rule in policy.get('mem_size', []):
        if re.search(rule['name'], name):
            return int(rule['bytes'])
    return 0


def build(policy, insts, report):
    ext_res = [re.compile(r) for r in policy['extensions']]
    xlen = int(policy.get('xlen', 64))
    acc_re = re.compile(policy.get('accumulates', '(?!)'))
    classes = []                       # in policy order
    for rule in policy['classes']:
        if rule['class'] not in [c['class'] for c in classes]:
            classes.append(rule)
    cls_index = {c['class']: i for i, c in enumerate(classes)}
    rows, skipped = [], {}
    for name, d in sorted(insts.items()):
        exts = extensions_of(d['definedBy'])
        if not any(r.search(e) for e in exts for r in ext_res):
            skipped.setdefault('extension', []).append(name); continue
        if not defined_for_xlen(d['definedBy'], xlen):
            skipped.setdefault('xlen', []).append(name); continue
        enc = d['encoding']
        if 'match' not in enc:
            enc = enc.get('RV%d' % xlen) or next(iter(enc.values()))
        match = enc['match'].replace(' ', '')
        length = len(match)
        assert length in (16, 32), (name, match)
        mask = int(''.join('0' if c == '-' else '1' for c in match), 2)
        mval = int(''.join('1' if c == '1' else '0' for c in match), 2)
        fields = [field_of(v, length, name) for v in (enc.get('variables') or [])]
        if len(fields) > MAX_FIELDS:
            sys.exit('%s: %d fields, raise MAX_FIELDS' % (name, len(fields)))
        for f in fields:
            if len(f['segs']) > MAX_SEGS:
                sys.exit('%s: field %s has %d segments, raise MAX_SEGS' % (name, f['name'], len(f['segs'])))
        asm = re.sub(r'\b(rs1|rs2|rs3|rd|frs1|frs2|frs3|frd)\b', lambda m: RENAME[m.group(1)], str(d.get('assembly', '')))
        rule = classify(policy, name, exts, asm)
        cls = rule['class']
        flags = set(rule.get('flags', []))
        if acc_re.search(name):
            for f in fields:
                if f['kind'] in ('XREG', 'FREG', 'VREG') and 'WRITE' in f['flags']:
                    f['flags'].append('READ')
        rows.append(dict(name=name, ext=exts[0], exts=exts, assembly=asm,
                         mask=mask, match=mval, length=length, fixed=bin(mask).count('1'),
                         cls=cls, cls_idx=cls_index[cls], fields=fields,
                         mem_size=mem_size(policy, name) if flags & {'load', 'store', 'prefetch', 'cache_flush'} else 0,
                         file=d['_file']))
    # Priority: longer encodings are distinguished by their low bits before
    # lookup; within a length, more fixed bits first (c.nop before c.addi,
    # c.ebreak before c.jalr), then by name for stability.
    rows.sort(key=lambda r: (r['length'], -r['fixed'], r['name']))
    for i, r in enumerate(rows):
        r['idx'] = i
    # Expected decode of each row's sample word (its match, variables zero):
    # the first row of the same length whose fixed bits agree.
    conflicts = []
    for r in rows:
        for s in rows:
            if s['length'] != r['length']:
                continue
            if (r['match'] & s['mask']) == s['match']:
                r['expect'] = s['idx']
                if s is not r and (s['mask'] & r['mask']) != r['mask']:
                    conflicts.append((r['name'], s['name']))   # s does not refine r's encoding: a cross overlap
                break
    if report:
        print('%d instructions from %d extensions; %d skipped (%s)' % (
            len(rows), len({e for r in rows for e in r['exts']}), sum(len(v) for v in skipped.values()),
            ', '.join('%s: %d' % (k, len(v)) for k, v in skipped.items())))
        bycls = {}
        for r in rows:
            bycls.setdefault(r['cls'], []).append(r['name'])
        for c in classes:
            names = bycls.get(c['class'], [])
            print('\n[%s] latency %s, %d instructions:' % (c['class'], c['latency'], len(names)))
            print('  ' + ' '.join(names))
        if conflicts:
            print('\nCROSS OVERLAPS (sample word of the first decodes as the second, which is not a refinement):')
            for a, b in conflicts:
                print('  %s -> %s' % (a, b))
        shadow = [(r['name'], rows[r['expect']]['name']) for r in rows if r['expect'] != r['idx'] and (r['name'], rows[r['expect']]['name']) not in conflicts]
        if shadow:
            print('\nrefinements (sample word hits a more specific sibling; expected): ' + ', '.join('%s->%s' % s for s in shadow))
        print('\nskipped for xlen: ' + ' '.join(skipped.get('xlen', [])))
    return rows, classes, conflicts


def describe_root(root):
    """A stable name for a UDB root: its basename, plus the commit when it is a git checkout."""
    import subprocess
    name = os.path.basename(os.path.abspath(root.rstrip('/')))
    try:
        rev = subprocess.run(['git', '-C', root, 'rev-parse', '--short', 'HEAD'], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, text=True, timeout=10).stdout.strip()
        if rev:
            return '%s@%s' % (name, rev)
    except Exception:
        pass
    parent = os.path.basename(os.path.dirname(os.path.abspath(root.rstrip('/'))))
    return '%s (%s)' % (name, parent) if name in ('udb', 'overlay') else name


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def emit(rows, classes, out_dir, roots, policy_path):
    hdr = ['/* Generated by tools/riscv/gen_isa.py -- do not edit.',
           ' * Sources: %s' % ', '.join(describe_root(r) for r in roots),
           ' * Policy:  %s */' % os.path.basename(policy_path)]
    h = hdr + ['#ifndef RISCV_ISA_GEN_H', '#define RISCV_ISA_GEN_H', '#include <stdint.h>', '', 'namespace rvisa {', '',
               'enum Class : uint8_t {']
    for c in classes:
        h.append('   CLASS_%s,' % c['class'].upper())
    h += ['   NUM_CLASSES', '};', '', 'enum Flag : uint32_t {']
    for i, f in enumerate(FLAG_BITS):
        h.append('   FLAG_%s = 1u << %d,' % (f.upper(), i))
    h += ['};', '', 'enum Cpi : uint8_t { %s };' % ', '.join('CPI_' + g.upper() for g in CPI_GROUPS), '',
          'enum FieldKind : uint8_t { %s };' % ', '.join('F_' + k for k in FIELD_KINDS),
          'enum FieldFlag : uint8_t { FF_READ = 1, FF_WRITE = 2, FF_PLUS8 = 4, FF_SEXT = 8 };', '',
          'const unsigned NUM_INSNS = %d;' % len(rows),
          'const unsigned MAX_FIELDS = %d;' % MAX_FIELDS,
          'const unsigned MAX_SEGS = %d;' % MAX_SEGS, '',
          '} // namespace rvisa', '#endif', '']
    open(os.path.join(out_dir, 'riscv_isa_gen.h'), 'w').write('\n'.join(h))

    c = hdr + ['#include "riscv_isa.h"', '', 'namespace rvisa {', '', 'const ClassInfo class_table[NUM_CLASSES] = {']
    for cl in classes:
        flags = ' | '.join('FLAG_' + f.upper() for f in cl.get('flags', [])) or '0'
        c.append('   { %s, %s, %d, %d, CPI_%s, %s },' % (
            cstr(cl['class']), cstr('perf_model/core/riscv/' + cl['class']), int(cl['latency']),
            int(cl.get('issue_cycles', 1)), cl.get('cpi', 'generic').upper(), flags))
    c += ['};', '', 'const Insn insn_table[NUM_INSNS] = {']
    for r in rows:
        fs = []
        for f in r['fields']:
            segs = ', '.join('{%d, %d}' % s for s in f['segs']) or '{0, 0}'
            fl = ' | '.join('FF_' + x for x in f['flags']) or '0'
            fs.append('{ %s, F_%s, %s, %d, %d, {%s} }' % (cstr(f['name']), f['kind'], fl, len(f['segs']), f['shift'], segs))
        c.append('   /* %4d */ { %s, %s, %s, 0x%08x, 0x%08x, %d, CLASS_%s, %d, %d, {%s} },' % (
            r['idx'], cstr(r['name']), cstr(r['ext']), cstr(r['assembly']), r['mask'], r['match'],
            r['length'], r['cls'].upper(), len(r['fields']), r['mem_size'], ', '.join(fs)))
    c += ['};', '']
    # dispatch buckets: 32-bit by bits 6:2, 16-bit by bits 1:0 and 15:13
    for length, key, nb in ((32, lambda m: (m >> 2) & 31, 32), (16, lambda m: (m & 3) | (((m >> 13) & 7) << 2), 32)):
        buckets = [[] for _ in range(nb)]
        for r in rows:
            if r['length'] != length:
                continue
            kmask = key(r['mask'])
            if kmask == nb - 1:
                buckets[key(r['match'])].append(r['idx'])
            else:                          # key bits not all fixed: every matching bucket
                for b in range(nb):
                    if (b & kmask) == key(r['match']):
                        buckets[b].append(r['idx'])
        flat, starts = [], []
        for b in buckets:
            starts.append(len(flat)); flat += b
        starts.append(len(flat))
        c.append('const uint16_t bucket%d_start[%d] = { %s };' % (length, nb + 1, ', '.join(map(str, starts))))
        c.append('const uint16_t bucket%d_list[%d] = { %s };' % (length, max(1, len(flat)), ', '.join(map(str, flat)) or '0'))
        c.append('')
    c.append('/* The row each row\'s sample word (match with variables zero) must decode to. */')
    c.append('const uint16_t expected_decode[NUM_INSNS] = { %s };' % ', '.join(str(r['expect']) for r in rows))
    c += ['', '} // namespace rvisa', '']
    open(os.path.join(out_dir, 'riscv_isa_gen.cc'), 'w').write('\n'.join(c))


def emit_config_block(classes, policy):
    lines = ['# BEGIN generated by tools/riscv/gen_isa.py from tools/riscv/isa_policy.yaml',
             '# The RISC-V decoder is generated from the RISC-V Unified Database; these are',
             '# its parameters and one placeholder latency per instruction class (cycles;',
             '# *_issue_cycles is how often the unit accepts a new one).',
             '[perf_model/core/riscv]']
    for k, v in (policy.get('decoder') or {}).items():
        lines.append('%s = %s' % (k, v))
    for c in classes:
        lines.append('%s_latency = %d' % (c['class'], int(c['latency'])))
        if 'issue_cycles' in c:
            lines.append('%s_issue_cycles = %d' % (c['class'], int(c['issue_cycles'])))
    lines.append('# END generated by tools/riscv/gen_isa.py')
    return '\n'.join(lines) + '\n'


def update_config(path, block):
    s = open(path).read()
    start, end = s.find('# BEGIN generated by tools/riscv/gen_isa.py'), s.find('# END generated by tools/riscv/gen_isa.py')
    if start >= 0 and end >= 0:
        end = s.index('\n', end) + 1
        s = s[:start] + block + s[end:]
    else:
        s = s.rstrip('\n') + '\n\n' + block
    open(path, 'w').write(s)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--udb', action='append', required=True, help='UDB root (repeatable; later roots override)')
    ap.add_argument('--policy', required=True)
    ap.add_argument('--out', default='decoder_lib')
    ap.add_argument('--config', action='append', default=[], help='config file(s) whose generated block to refresh')
    ap.add_argument('--report', action='store_true')
    args = ap.parse_args()
    policy = yaml.safe_load(open(args.policy))
    rows, classes, conflicts = build(policy, load_insts(args.udb), args.report)
    emit(rows, classes, args.out, args.udb, args.policy)
    for cfg in args.config:
        update_config(cfg, emit_config_block(classes, policy))
    print('wrote %s/riscv_isa_gen.{h,cc}: %d instructions, %d classes%s' % (
        args.out, len(rows), len(classes), ', %d cross overlaps' % len(conflicts) if conflicts else ''))
    return 1 if conflicts else 0


if __name__ == '__main__':
    sys.exit(main())
