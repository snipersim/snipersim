#!/usr/bin/env python3
"""Virtuoso v2 integration suite: prove that additions changed nothing else.

The Virtuoso v2 port (TRAIL, Revelator, Victima, MimicOS v2) lands as a long
series of steps, most of which are supposed to be invisible to every existing
configuration -- new files nothing selects, or subsystems reachable only by
naming them in a config.  "Supposed to be" is the problem: the way that goes
wrong is a shared header or a hot path picking up a change nobody meant to make,
and the symptom is a number moving in a configuration nobody was looking at.

So the suite has two halves:

  --invariance   run the existing smoke configs and require their statistics to
                 be *identical* to a recorded baseline.  This is the safety
                 argument for landing orthogonal work first, and it runs after
                 every step.

  --feature      per-feature checks, added by the step that makes each feature
                 runnable.  Each asserts on performance counters the feature is
                 specified to move, which is enough to tell "wired up and
                 working" from "compiles and does nothing".

Determinism, and how much of it there is
----------------------------------------
Sniper is very nearly reproducible for a fixed build, config and workload, and
the residual noise was measured rather than assumed.  With ASLR on, two runs of
`baseline` differ in 257 of 2346 statistics -- the address-sensitive ones
(access-mru, data-reuse), so every run here goes through `setarch -R`.

With ASLR pinned, five identical runs moved 6 counters out of 2603.  Four are
host wall-clock readings and are dropped outright (VOLATILE).  The other two are
`L2.store-overlapping-misses{,-data}`, which moved by exactly 1 on a value of
19 -- a genuine simulation counter that is simply not stable to the last count.

So the comparison is numeric with a tolerance rather than exact: a counter
passes when it is within TOL_ABS counts or TOL_REL of the baseline.  The
defaults sit just above the measured floor, which keeps the check able to notice
a real change (an unintended behaviour change moves many counters, or moves one
by far more than a count) without failing on the one that will not sit still.

The workload is built at a fixed path, and that matters more than it looks.
With ASLR off the guest's addresses are determined by its environment, and the
program path is part of that: the same binary run from two paths of different
lengths differs in 303 of 2342 counters here.  Compiling it into a fresh
temporary directory per run -- which this suite did at first -- therefore
produced a different answer every invocation and made the comparison
meaningless.

Baselines are local, not committed: they depend on the host toolchain (the
workload is compiled here) so a checked-in copy would be noise on any other
machine.  Bless once at the start of a series, check after every step:

    testing/test_virtuoso.py --bless          # at the base commit
    ...apply a step...
    testing/test_virtuoso.py                  # must be clean
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SNIPER_ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(SNIPER_ROOT, 'tools'))

from common.failfast import FailFast, add_argument as add_failfast
from common.runlog import RunLog

WORKLOAD_SRC = os.path.join(HERE, 'apps', 'workload.c')
WORKLOAD_OUTPUT = re.compile(r'workload isum=1419064784 fsum=100638720\.000')

# Where blessed baselines live.  Gitignored: see the module docstring.
GOLDEN_DIR = os.path.join(HERE, 'goldens', 'virtuoso')

# Host measurements, not simulation results.  These differ run to run by
# construction and say nothing about whether behaviour changed.
VOLATILE = ('roi.ipscore', 'roi.ipstotal', 'roi.walltime', 'time.walltime')

# Measured noise floor is 1 count on one counter; these sit just above it.
TOL_ABS = 2          # counts; absorbs the L2.store-overlapping-misses jitter
TOL_REL = 0.001      # 0.1%; every other counter was stable to the last digit

# The configurations that must not move.  Deliberately the plain ones: none of
# them exercises anything Virtuoso adds, which is exactly why they are the
# canary for a change leaking out of a new subsystem.
INVARIANCE = [
    ('baseline',            ['address_translation_schemes/baseline'],            1),
    ('parametric_baseline', ['address_translation_schemes/parametric_baseline'], 1),
    ('userspace_baseline',  ['address_translation_schemes/userspace_baseline'],  1),
    ('parametric_4c',       ['address_translation_schemes/multicore/'
                             'parametric_baseline_4c'],                          4),
]



def counter(stats, name):
    """A statistic as a plain int, summed over cores.  0 when absent."""
    v = stats.get(name)
    if v is None:
        return 0
    if isinstance(v, (list, tuple)):
        return sum(int(x) for x in v)
    return int(v)


def require_nonzero(*names):
    """Check builder: every named counter must exist and be non-zero."""
    def check(stats):
        missing = [n for n in names if n not in stats]
        if missing:
            return False, 'counter not registered: %s' % ', '.join(missing)
        zero = [n for n in names if counter(stats, n) == 0]
        if zero:
            return False, 'counter stayed zero: %s' % ', '.join(zero)
        return True, ', '.join('%s=%d' % (n, counter(stats, n)) for n in names)
    return check


# Feature checks.  Each entry is (config list, ncores, check) where check takes
# the statistics dict and returns (ok, detail).  Populated by the integration
# steps as they land; see VIRTUOSO_V2_INTEGRATION.md.
FEATURES = {
    # O5: the detailed DRAM model's per-stage queueing breakdown.  baseline
    # drives real DRAM traffic, so every stage should accumulate; a stage stuck
    # at zero means its accumulation site was missed.
    'dram_queueing': (
        ['address_translation_schemes/baseline'], 1,
        require_nonzero('ddr.total-bank-pending-wait-data',
                        'ddr.total-rank-avail-delay-data',
                        'ddr.total-bank-group-avail-delay-data',
                        'ddr.total-dram-bus-queue-delay-data')),

    # O5: the radix page table counts prefetch-walk faults apart from demand
    # faults.  Nothing prefetches yet, so the counter must exist and read zero
    # while demand faults keep being counted -- which is what tells "registered"
    # apart from "absent".
    'radix_fault_split': (
        ['address_translation_schemes/baseline'], 1,
        lambda stats: (
            ('radix_4level.prefetch_page_faults' in stats
             and counter(stats, 'radix_4level.prefetch_page_faults') == 0
             and counter(stats, 'radix_4level.page_faults') > 0),
            'demand=%d prefetch=%d' % (counter(stats, 'radix_4level.page_faults'),
                                       counter(stats, 'radix_4level.prefetch_page_faults'))
            if 'radix_4level.prefetch_page_faults' in stats
            else 'prefetch_page_faults not registered')),
}


# Fixed, so that the guest's environment -- and therefore its addresses -- are
# the same on every run.  See the module docstring.
WORKLOAD_BIN = os.path.join(GOLDEN_DIR, 'workload')


def build_workload():
    os.makedirs(GOLDEN_DIR, exist_ok=True)
    if (not os.path.exists(WORKLOAD_BIN)
            or os.path.getmtime(WORKLOAD_BIN) < os.path.getmtime(WORKLOAD_SRC)):
        cc = os.environ.get('CC', 'gcc')
        subprocess.run([cc, '-O1', '-o', WORKLOAD_BIN, WORKLOAD_SRC], check=True)
    return WORKLOAD_BIN


def have_setarch():
    return shutil.which('setarch') is not None


def run_sniper(names, ncores, outdir, workload, timeout):
    """One simulation, with ASLR pinned so the statistics are reproducible."""
    shutil.rmtree(outdir, ignore_errors=True)
    cmd = []
    if have_setarch():
        cmd += ['setarch', '-R']
    cmd += [os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', str(ncores)]
    cmd += ['-c' + n for n in names]
    cmd += ['-d', outdir, '--', workload]
    try:
        p = subprocess.run(cmd, timeout=timeout, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, cwd=SNIPER_ROOT,
                           start_new_session=True)
    except subprocess.TimeoutExpired:
        return None, 'timed out after %ds' % timeout
    if p.returncode != 0:
        err = re.search(r'\*ERROR\*\s*(.{0,60})', p.stdout)
        return None, err.group(1).strip() if err else 'exit %d' % p.returncode
    # run-sniper exits 0 even when the simulator refuses to start.
    if not WORKLOAD_OUTPUT.search(p.stdout):
        return None, 'exit 0 but the workload produced no output'
    return p.stdout, None


def read_stats(outdir):
    import sniper_lib
    return sniper_lib.get_results(resultsdir=outdir)['results']


def comparable(stats):
    """The part of a statistics dict that is a property of the simulation."""
    return {k: v for k, v in stats.items() if k not in VOLATILE}


def golden_path(name):
    return os.path.join(GOLDEN_DIR, name + '.json')


def bless(name, stats, workload):
    os.makedirs(GOLDEN_DIR, exist_ok=True)
    rec = {
        'stats': {k: repr(v) for k, v in sorted(comparable(stats).items())},
        'blessed_at': time.strftime('%Y-%m-%d %H:%M:%S'),
        'commit': git_head(),
        'workload_sha1': file_sha1(workload),
    }
    with open(golden_path(name), 'w') as f:
        json.dump(rec, f, indent=1, sort_keys=True)
    return len(rec['stats'])


def as_numbers(v):
    """A statistic as a list of floats, or None if it is not numeric."""
    seq = v if isinstance(v, (list, tuple)) else [v]
    out = []
    for x in seq:
        try:
            out.append(float(x))
        except (TypeError, ValueError):
            return None
    return out


def deviation(want, got):
    """How far one counter moved, as (absolute, relative) worst over cores.

    Returns None when the two are not comparable at all -- a different number of
    cores, or a value that is not a number -- which is treated as a change.
    """
    a, b = as_numbers(want), as_numbers(got)
    if a is None or b is None or len(a) != len(b):
        return None
    worst = (0.0, 0.0)
    for x, y in zip(a, b):
        d = abs(y - x)
        r = d / abs(x) if x else (0.0 if d == 0 else float('inf'))
        if (d, r) > worst:
            worst = (d, r)
    return worst


def compare(name, stats, tol_abs=TOL_ABS, tol_rel=TOL_REL):
    """Compare against the blessed baseline, within tolerance."""
    path = golden_path(name)
    if not os.path.exists(path):
        return None, 'no baseline (run --bless at the base commit)'
    rec = json.load(open(path))
    want = rec['stats']
    got = {k: repr(v) for k, v in comparable(stats).items()}

    added = sorted(set(got) - set(want))
    removed = sorted(set(want) - set(got))

    outside = []          # (abs, rel, key) beyond tolerance
    inside = 0            # moved, but within tolerance
    for k in sorted(set(got) & set(want)):
        if got[k] == want[k]:
            continue
        dev = deviation(_unrepr(want[k]), _unrepr(got[k]))
        if dev is None:
            outside.append((float('inf'), float('inf'), k))
            continue
        d, r = dev
        if d > tol_abs and r > tol_rel:
            outside.append((d, r, k))
        else:
            inside += 1

    if outside or removed:
        bits = []
        if outside:
            outside.sort(reverse=True)
            d, r, k = outside[0]
            bits.append('%d counters outside tolerance (worst %s: %s -> %s, '
                        '%+.3g = %.2g%%)'
                        % (len(outside), k, want[k][:18], got[k][:18], d, 100 * r))
        if removed:
            bits.append('%d counters disappeared (e.g. %s)'
                        % (len(removed), removed[0]))
        return False, '; '.join(bits)

    note = '%d counters' % len(got)
    if inside:
        note += ', %d jittered within tolerance' % inside
    if added:
        note += ', %d new' % len(added)
    return True, note


def _unrepr(s):
    """Recover a value from the repr() stored in a baseline."""
    try:
        import ast
        return ast.literal_eval(s)
    except (ValueError, SyntaxError):
        return s


def git_head():
    try:
        return subprocess.run(['git', 'rev-parse', '--short', 'HEAD'],
                              cwd=SNIPER_ROOT, capture_output=True, text=True,
                              check=True).stdout.strip()
    except Exception:
        return '?'


def file_sha1(path):
    return hashlib.sha1(open(path, 'rb').read()).hexdigest()[:12]


def main():
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass

    ap = argparse.ArgumentParser()
    ap.add_argument('--bless', action='store_true',
                    help='record the current statistics as the baseline')
    ap.add_argument('--feature', action='append',
                    help='run only this feature check (repeatable)')
    ap.add_argument('--only', action='append',
                    help='run only this invariance config (repeatable)')
    ap.add_argument('--timeout', type=int, default=600)
    ap.add_argument('--tol-abs', type=float, default=TOL_ABS,
                    help='counters may differ by this many counts (default %d)' % TOL_ABS)
    ap.add_argument('--tol-rel', type=float, default=TOL_REL,
                    help='...or by this fraction (default %g)' % TOL_REL)
    add_failfast(ap)
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(SNIPER_ROOT, 'lib', 'sniper')):
        print('error: lib/sniper not built; run make first', file=sys.stderr)
        return 1
    if not have_setarch():
        print('warning: setarch not found; ASLR stays on and statistics will '
              'not be reproducible', file=sys.stderr)

    log = RunLog('virtuoso')
    workdir = tempfile.mkdtemp(prefix='sniper-virtuoso-')
    failures = 0
    stopped_early = False
    try:
        workload = build_workload()

        cases = INVARIANCE
        if args.only:
            cases = [c for c in cases if c[0] in args.only]
        if args.feature:                 # --feature implies feature checks only
            cases = []

        for name, cfgs, ncores in cases:
            print('%-22s ' % name, end='')
            sys.stdout.flush()
            started = time.time()
            outdir = os.path.join(workdir, 'inv-' + name)
            out, err = run_sniper(cfgs, ncores, outdir, workload, args.timeout)
            took = time.time() - started
            if err:
                log.record('invariance:' + name, took, 'FAIL')
                print('FAIL: %s [%ds]' % (err, round(took)))
                failures += 1
                if args.failfast:
                    raise FailFast()
                continue
            stats = read_stats(outdir)
            if args.bless:
                n = bless(name, stats, workload)
                log.record('bless:' + name, took)
                print('blessed %d counters [%ds]' % (n, round(took)))
            else:
                ok, detail = compare(name, stats, args.tol_abs, args.tol_rel)
                log.record('invariance:' + name, took,
                           'PASS' if ok is not False else 'FAIL')
                if ok is None:
                    print('SKIP (%s) [%ds]' % (detail, round(took)))
                elif ok:
                    print('PASS (%s) [%ds]' % (detail, round(took)))
                else:
                    print('FAIL: %s [%ds]' % (detail, round(took)))
                    failures += 1
                    if args.failfast:
                        raise FailFast()
            shutil.rmtree(outdir, ignore_errors=True)

        names = args.feature if args.feature else sorted(FEATURES)
        for fname in names:
            if fname not in FEATURES:
                print('%-22s SKIP (no such feature check)' % fname)
                continue
            cfgs, ncores, check = FEATURES[fname]
            print('%-22s ' % fname, end='')
            sys.stdout.flush()
            started = time.time()
            outdir = os.path.join(workdir, 'feat-' + fname)
            out, err = run_sniper(cfgs, ncores, outdir, workload, args.timeout)
            took = time.time() - started
            if err:
                log.record('feature:' + fname, took, 'FAIL')
                print('FAIL: %s [%ds]' % (err, round(took)))
                failures += 1
                if args.failfast:
                    raise FailFast()
                continue
            ok, detail = check(read_stats(outdir))
            log.record('feature:' + fname, took, 'PASS' if ok else 'FAIL')
            print('%s (%s) [%ds]' % ('PASS' if ok else 'FAIL', detail, round(took)))
            if not ok:
                failures += 1
                if args.failfast:
                    raise FailFast()
            shutil.rmtree(outdir, ignore_errors=True)

    except FailFast:
        stopped_early = True
    finally:
        if stopped_early:
            print('\nstopped at the first failure; work directory kept at %s'
                  % workdir)
        else:
            shutil.rmtree(workdir, ignore_errors=True)
        log.finish()

    print()
    ran = len(log.checks)
    if failures:
        print('%d of %d checks FAILED' % (failures, ran))
        return 1
    print('all %d checks passed' % ran)
    return 0


if __name__ == '__main__':
    sys.exit(main())
