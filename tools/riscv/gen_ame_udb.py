#!/usr/bin/env python3
"""Build a UDB overlay for the RISC-V Attached Matrix Extension (AME, "Ztt")
from the working group's specification source.

    gen_ame_udb.py --ame <riscv-attached-matrix-extension checkout> \
                   --out tools/riscv/overlay

AME's public repository is AsciiDoc, not UDB YAML, but its instruction
chapter is machine-readable: every instruction carries a wavedrom field
list (its exact encoding, fixed fields as values, operands as names), a
mnemonic line (the operand order), a synopsis, a description and an
operation block; the repository also ships a generated mask/match table
(INSTRUCTION_ENCODINGS.md) that this script checks its own encodings
against.  Until the group publishes AME in UDB form, this converter is the
UDB source: it writes spec/std/isa/inst/Ztt/<mnemonic>.yaml and
spec/std/isa/ext/Ztt.yaml in the layout gen_isa.py reads, so AME goes down
the same path as IME.

Operand naming follows the UDB convention that encodes direction in the
name (xd/xs1/xs2 for X registers): the spec's md/ms1/ms2 for M registers
and ad/as1 for accumulators are kept, and the spec's rd/rs1/rs2 become
xd/xs1/xs2.  A destination that is also read (the *acc* families) is
marked in the description; gen_isa.py's accumulate rule reads it.  The
operation blocks are the spec's own pseudocode, not UDB IDL; they are
carried verbatim for reference."""
import pathlib
import io
import argparse, json, os, re, subprocess, sys

ATTR_RE = re.compile(r'^:([a-z0-9-]+):\s*(0x[0-9a-fA-F]+|\d+)', re.M)


def attrs(root):
    t = pathlib.Path(os.path.join(root, 'src', 'instruction-encoding-allocations.adoc')).read_text()
    return {k: int(v, 0) for k, v in ATTR_RE.findall(t)}


def blocks(root):
    """Split instructions.adoc into per-instruction blocks."""
    t = pathlib.Path(os.path.join(root, 'src', 'instructions.adoc')).read_text()
    parts = re.split(r'^=== `([^`]+)`\s*$', t, flags=re.M)
    out = []
    for i in range(1, len(parts), 2):
        out.append((parts[i].strip(), parts[i + 1]))
    return out


def field(s):
    """One 'Field::' paragraph: the text after the marker up to the next marker."""
    return s.strip()


def parse_block(name, body, at):
    def section(label):
        m = re.search(r'^' + label + r'::\s*\n(.*?)(?=^\S[^\n]*::\s*$|^\[#|^<<<|\Z)', body, flags=re.M | re.S)
        return m.group(1) if m else ''
    synopsis = ' '.join(section('Synopsis').split())
    mnem = re.search(r'`([^`]+)`', section('Mnemonic'))
    assembly = mnem.group(1).strip() if mnem else name
    ops = assembly.split(None, 1)[1] if ' ' in assembly else ''
    # wavedrom: LSB-first list of {bits, name}; fixed names are numbers or {attr}
    wm = re.search(r'\{"reg":\s*\[(.*?)\]\s*\}', body, flags=re.S)
    if not wm:
        sys.exit('%s: no wavedrom encoding' % name)
    raw = '[' + wm.group(1) + ']'
    raw = re.sub(r'\{([a-z0-9-]+)\}', lambda m: str(at[m.group(1)]), raw)
    raw = re.sub(r'"name":\s*(0x[0-9a-fA-F]+)', lambda m: '"name": %d' % int(m.group(1), 16), raw)
    fields = json.loads(raw)
    desc = ' '.join(section('Description').split())
    op = section('Operation')
    opm = re.search(r'----\n(.*?)\n----', op, flags=re.S)
    operation = opm.group(1).strip() if opm else op.strip()
    return synopsis, assembly, ops, fields, desc, operation


KIND = {'m': 'M register', 'a': 'accumulator register', 'x': 'X register'}


def rename(op):
    """Spec operand -> UDB-style name with direction; returns (new, kind)."""
    op = op.strip('()')
    m = re.match(r'^(md|ms\d?|acc|ad|as\d?|rd|rs\d?)$', op)
    if not m:
        return op, None
    if op == 'rd': return 'xd', 'x'
    if op.startswith('rs'): return 'xs' + (op[2:] or '1'), 'x'
    if op == 'md': return 'md', 'm'
    if op == 'ms': return 'ms1', 'm'
    if op.startswith('ms'): return op, 'm'
    if op in ('ad', 'acc'): return 'ad', 'a'
    if op.startswith('as'): return op, 'a'
    return op, None


def convert(root, out):
    at = attrs(root)
    rev = subprocess.run(['git', '-C', root, 'rev-parse', '--short', 'HEAD'], stdout=subprocess.PIPE, text=True).stdout.strip()
    inst_dir = os.path.join(out, 'spec', 'std', 'isa', 'inst', 'Ztt')
    ext_dir = os.path.join(out, 'spec', 'std', 'isa', 'ext')
    os.makedirs(inst_dir, exist_ok=True); os.makedirs(ext_dir, exist_ok=True)
    for f in os.listdir(inst_dir):
        os.remove(os.path.join(inst_dir, f))
    # the repository's own mask/match table, for the cross-check
    table = {}
    for line in io.StringIO(pathlib.Path(os.path.join(root, 'INSTRUCTION_ENCODINGS.md')).read_text()):
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        hexes = [c for c in cells if re.match(r'^`?0x[0-9a-fA-F]{8}`?$', c)]
        names = [c.strip('`') for c in cells if re.match(r'^`?[a-z][a-z0-9.]*`?$', c)]
        if len(hexes) >= 2 and names:
            table[names[0]] = tuple(int(h.strip('`'), 16) for h in hexes[:2])
    written, mismatches = 0, []
    for name, body in blocks(root):
        synopsis, assembly, ops, fields, desc, operation = parse_block(name, body, at)
        # match string and variables from the LSB-first field list
        bits = ['-'] * 32
        variables = []
        lsb = 0
        # operand order in the mnemonic decides direction: first is the destination
        order = [o.strip() for o in ops.split(',')] if ops else []
        renamed = {}
        # direction follows position: the first operand is the destination
        # (unless the spec already names it a source, as stores do), every
        # later one a source -- so `agettyp xd, ad` reads as1, not ad
        nsrc = {'m': 0, 'a': 0, 'x': 0}
        for i, o in enumerate(order):
            new, kind = rename(o)
            if not kind:
                continue
            if i > 0 and new.endswith('d'):
                nsrc[kind] += 1
                new = kind + 's%d' % nsrc[kind]
            elif new.endswith('s1') or new.endswith('s2'):
                nsrc[kind] = max(nsrc[kind], int(new[-1]))
            renamed[o.strip('()')] = new
        seen_src = {}
        for f in fields:
            n = f['bits']; nm = f['name']
            if isinstance(nm, int):
                for b in range(n):
                    bits[lsb + b] = '1' if (nm >> b) & 1 else '0'
            else:
                new = renamed.get(nm)
                if new is None:
                    new, kind = rename(nm)
                # sources that the mnemonic lists after the destination
                variables.append({'name': new, 'location': '%d-%d' % (lsb + n - 1, lsb)})
            lsb += n
        assert lsb == 32, (name, lsb)
        match = ''.join(reversed(bits))
        mask = int(''.join('0' if c == '-' else '1' for c in match), 2)
        mval = int(''.join('1' if c == '1' else '0' for c in match), 2)
        if name in table and table[name] != (mask, mval):
            mismatches.append((name, table[name], (mask, mval)))
        asm_new = ', '.join(('(' + renamed.get(o.strip('()'), o.strip('()')) + ')') if o.startswith('(') else renamed.get(o, o) for o in order)
        accumulates = 'acc' in name.replace('.', '')
        fam = ('backend management' if name.startswith('ame.') else 'datatype configuration' if name.endswith('typ')
               else 'memory' if name.startswith(('mls.', 'mss.')) else 'matrix multiply' if name.endswith('.2d')
               else 'move' if name.startswith(('mmov', 'mmove', 'mbcast', 'mzero')) else 'elementwise/reduction')
        y = ['# Generated by tools/riscv/gen_ame_udb.py from riscv-attached-matrix-extension@%s' % rev,
             '# (src/instructions.adoc, provisional encodings).  Do not edit; regenerate.',
             '$schema: "inst_schema.json#"', 'kind: instruction', 'name: %s' % name,
             'long_name: %s' % json.dumps(synopsis or name),
             'description: |']
        y += ['  ' + l for l in (desc or synopsis).split('. ') if l]
        y += ['  Family: %s.%s' % (fam, '  The destination is also a source (accumulating).' if accumulates else ''),
              'definedBy:', '  extension:', '    name: Ztt',
              'assembly: %s' % json.dumps(asm_new) if asm_new else 'assembly: ""',
              'encoding:', '  match: "%s"' % match, '  variables:']
        for v in variables:
            y += ['    - name: %s' % v['name'], '      location: %s' % v['location']]
        if not variables:
            y[-1] = '  variables: []'
        y += ['access:', '  s: always', '  u: always', '  vs: always', '  vu: always',
              'data_independent_timing: false',
              '# The specification\'s pseudocode, carried for reference (not UDB IDL):',
              'operation(): |']
        y += ['  ' + l for l in (operation or 'see the specification').splitlines()]
        pathlib.Path(os.path.join(inst_dir, name + '.yaml')).write_text('\n'.join(y) + '\n')
        written += 1
    ext = ['# Generated by tools/riscv/gen_ame_udb.py from riscv-attached-matrix-extension@%s' % rev,
           '$schema: "ext_schema.json#"', 'kind: extension', 'name: Ztt',
           'long_name: Attached Matrix Extension (AME)', 'type: unprivileged',
           'description: |',
           '  RISC-V Attached Matrix Extension, working-group draft: a self-contained matrix unit with',
           '  its own M registers (AME_NUM_M_REGS = 16 or 32, each AME_UNIT_DATATYPE_SIZE * AME_NELEM',
           '  bits), accumulator registers (AME_NUM_ACC_REGS), seven CSRs (amenlen, ameudsz, amestype,',
           '  ameown, amefflags, amexsat, amestatus), backend ownership (ame.acquire / ame.release),',
           '  loads and stores between memory and M registers (mls.*, mss.*), matrix multiplies',
           '  (*.2d), elementwise, reduction, move and datatype-configuration instructions.',
           '  Encodings are provisional (CUSTOM-1 opcode, funct3 = 000, funct7 banks) until the',
           '  extension receives its RISC-V allocation; regenerate when the draft moves.',
           'versions:', '  - version: "0.0.0-draft"', '    state: development',
           '    ratification_date: null', '    source: riscv-attached-matrix-extension@%s' % rev]
    pathlib.Path(os.path.join(ext_dir, 'Ztt.yaml')).write_text('\n'.join(ext) + '\n')
    print('wrote %d instructions to %s (+ ext/Ztt.yaml); %d checked against INSTRUCTION_ENCODINGS.md, %d mismatches'
          % (written, inst_dir, sum(1 for n, _ in blocks(root) if n in table), len(mismatches)))
    for n, want, got in mismatches[:8]:
        print('  MISMATCH %s: table mask/match %08x/%08x, wavedrom %08x/%08x' % (n, want[0], want[1], got[0], got[1]))
    return 1 if mismatches else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ame', required=True, help='riscv-attached-matrix-extension checkout')
    ap.add_argument('--out', default='tools/riscv/overlay')
    a = ap.parse_args()
    return convert(a.ame, a.out)


if __name__ == '__main__':
    sys.exit(main())
