#!/usr/bin/env python3
"""
Regression test: every shipped configuration still runs.

Nothing in the tree exercised the shipped configs, so a change to
config/base.cfg -- which run-sniper loads before every simulation -- could
silently break machines nobody happened to try.  This runs them all.

Two things this test is deliberately strict about, both learned the hard way:

  * Success is the process exit code, not a string in the log.  A run that
    aborts with SIGABRT still prints "[SNIPER] End", so grepping for it reports
    broken configurations as healthy.

  * Core count matters.  Configs exercise different code paths at different core
    counts -- tag-directory placement, NUCA, per-core MMU -- so each is checked
    at more than one.

  * The workload has to be an ordinary program.  This used to run /bin/true,
    which does no real work, touches almost no memory and exits before a frontend
    has much to instrument; Sniper does not handle it well, so a result there
    describes the corner case rather than the configuration.  apps/workload.c
    allocates, walks memory, mixes integer and floating-point work and calls into
    libc, and prints a checksum so a run that quietly produced nothing is not
    mistaken for one that worked.

Every configuration is run twice: once with --fast-forward and once with the
timing model attached.  The suite used to do only the first, which meant the
thing Sniper exists to do had no coverage: a configuration could deadlock or
abort the instant a core model was attached and still report a clean run.

Usage:
  python3 testing/test_configs.py
  python3 testing/test_configs.py --only gainestown
  python3 testing/test_configs.py --cores 1,4,8
  python3 testing/test_configs.py --mode detailed
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SNIPER_ROOT = os.path.dirname(HERE)
CONFIG_DIR = os.path.join(SNIPER_ROOT, 'config')
WORKLOAD_SRC = os.path.join(HERE, 'apps', 'workload.c')

# Printed by the workload once it has finished; its absence means the run
# produced nothing even if the exit status was zero.
WORKLOAD_OUTPUT = re.compile(r'workload isum=1419064784 fsum=100638720\.000')

# 1 and 2, not 1 and 4: gainestown gives its L3 shared_cores = 4, and at -n 4 the
# tag-directory placement check rejects the topology --
#   *ERROR* Tag directories may only be at 'master' node of shared caches
# -- so a quad-core Xeon config cannot be run with four cores.  That is a real
# wart, tracked separately; it is not something this suite should trip over on
# every config layered onto gainestown.
DEFAULT_CORES = (1, 2)

# Complete machines: must run on their own.
EXPECT_PASS = [
    'dunnington', 'dunnington16', 'gainestown', 'nehalem-lite', 'silvermont',
]

# Overlay fragments: tune one aspect and must be layered on a complete config.
# default.cfg is an empty file and smt1.cfg is two lines, so running them alone
# was never meaningful.
EXPECT_PASS_LAYERED = [
    'atd', 'cacheonly', 'default', 'dram-cache', 'interval', 'jikes', 'llc-qbs',
    'nehalem', 'noc', 'nuca-cache', 'oneipc', 'prefetcher', 'rob',
    'smt1', 'smt2', 'smt4', 'smt8',
]
LAYER_BASE = 'gainestown'
LAYER_BASE_BY_MODE = {}

# oneipc replaces the core model, and on gainestown's topology that trips the
# same tag-directory placement check as gainestown -n 4.  It layers cleanly onto
# nehalem-lite, so test it there rather than pretend it is broken.
LAYER_BASE_OVERRIDE = {
    'oneipc': 'nehalem-lite',
}

# Cannot work in a plain x86 SDE build, with the reason.  Listed rather than
# skipped, so that one starting to work is reported too.
EXPECT_FAIL = {
    'beckton': 'cache sizes are not a power of two '
               '("Caches of non-power of 2 size need funky hash function")',
    'cortex-a53': 'ARM target: needs an ARM frontend and an ARM trace',
    'cortex-a72': 'ARM target: needs an ARM frontend and an ARM trace',
    'kingscross': 'mesh topology fixed at 16:4, so it needs 64 cores',
    'riscv': 'needs a RISC-V build ("Unknown core model riscv")',
    'riscv-mediumboom': 'needs a RISC-V build ("Unknown core model riscv")',
    'sampling': 'sampling is only supported with the Pin frontend',
}

# Modes.  Until this suite grew the second one it ran everything with
# --fast-forward, so detailed timing -- what Sniper is for -- had no coverage at
# all, and configurations that deadlock or abort the moment a timing model is
# attached passed every run.
MODES = ('fast-forward', 'detailed')

# Known to fail once a timing model is attached, with the reason.  Keyed by
# config name: a case is expected to fail in detailed mode if any of the configs
# it layers is listed here.
#
# Empty at the moment, and it was not always: every config with a cache shared
# between cores (shared_cores > 1) under parametric_dram_directory_msi --
# gainestown, dunnington, dunnington16, silvermont, and nehalem-lite once
# nehalem was layered back onto it -- deadlocked the first time a page-table
# walk missed to the directory.  A pass-through metadata request skipped the
# set-lock downgrade along with the data copy, leaking shared_cores-1 mutexes
# to the network thread; see the comment at the release in cache_cntlr.cc.
# If a config lands back here with "timed out", look there first.
EXPECT_FAIL_DETAILED = {}

# Not a configuration to run on its own.
EXCLUDE = {'base'}


def shipped_configs():
    return sorted(os.path.splitext(f)[0]
                  for f in os.listdir(CONFIG_DIR) if f.endswith('.cfg'))


def build_workload(workdir):
    exe = os.path.join(workdir, 'workload')
    cc = os.environ.get('CC', 'gcc')
    subprocess.run([cc, '-O1', '-o', exe, WORKLOAD_SRC], check=True)
    return exe


def run_config(names, ncores, workdir, workload, timeout, mode):
    tag = '+'.join(names)
    outdir = os.path.join(workdir, 'out-%s-%s-n%d' % (tag, mode, ncores))
    shutil.rmtree(outdir, ignore_errors=True)
    cmd = [os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', str(ncores)]
    if mode == 'fast-forward':
        cmd.append('--fast-forward')
    cmd += ['-c' + n for n in names]
    cmd += ['-d', outdir, '--', workload]
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout, text=True, cwd=SNIPER_ROOT)
    except subprocess.TimeoutExpired:
        return False, 'timed out after %ds' % timeout

    # run-sniper exits 0 even when the simulator refuses to start, so the exit
    # status alone is not enough: require the workload's own output too.
    ok = p.returncode == 0 and bool(WORKLOAD_OUTPUT.search(p.stdout))
    if ok:
        shutil.rmtree(outdir, ignore_errors=True)
        return True, 'ok'

    missing = re.search(r'Configuration value (\S+) not found', p.stdout)
    if missing:
        return False, 'missing config value %s' % missing.group(1)
    err = re.search(r'\*ERROR\*\s*(.{0,60})', p.stdout)
    if err:
        return False, err.group(1).strip()
    if p.returncode == 0:
        return False, 'exit 0 but the workload produced no output'
    return False, 'exit %d' % p.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', action='append', help='check only this config (repeatable)')
    ap.add_argument('--cores', default=','.join(str(c) for c in DEFAULT_CORES),
                    help='comma-separated core counts (default: %(default)s)')
    ap.add_argument('--timeout', type=int, default=300)
    # A healthy detailed run of this workload is about ten seconds, so a
    # configuration that has not finished in two minutes has deadlocked.  The
    # shorter limit matters because the configs that do deadlock spend the whole
    # timeout proving it.
    ap.add_argument('--timeout-detailed', type=int, default=120)
    ap.add_argument('--mode', action='append', choices=MODES,
                    help='run only this mode (repeatable; default: both)')
    args = ap.parse_args()

    if not os.path.exists(os.path.join(SNIPER_ROOT, 'lib', 'sniper')):
        print('error: lib/sniper not built; run make first', file=sys.stderr)
        return 2

    cores = [int(c) for c in args.cores.split(',')]
    classified = set(EXPECT_PASS) | set(EXPECT_PASS_LAYERED) | set(EXPECT_FAIL) | EXCLUDE
    unclassified = [c for c in shipped_configs() if c not in classified]

    def build_cases(mode):
        layer_base = LAYER_BASE_BY_MODE.get(mode, LAYER_BASE)
        out = []   # (label, [config names], expect_ok, note)
        for n in EXPECT_PASS:
            out.append((n, [n], True, None))
        for n in EXPECT_PASS_LAYERED:
            base = LAYER_BASE_OVERRIDE.get(n, layer_base)
            if base == n:          # nehalem-lite layered onto itself is not a case
                continue
            out.append(('%s + %s' % (base, n), [base, n], True, None))
        for n in sorted(EXPECT_FAIL):
            out.append((n, [n], False, EXPECT_FAIL[n]))
        if args.only:
            out = [c for c in out if any(o in c[1] for o in args.only)]
        return out

    modes = args.mode or list(MODES)

    workdir = tempfile.mkdtemp(prefix='sniper-configs-')
    failures = []
    checks = 0
    try:
        workload = build_workload(workdir)
        for mode in modes:
            print('=== %s ===' % mode)
            for label, names, expect_ok, note in build_cases(mode):
                # A configuration can be fine in fast-forward and broken once a
                # timing model is attached; that is the whole reason this suite
                # runs both.
                mode_note = None
                if mode == 'detailed':
                    for n in names:
                        if n in EXPECT_FAIL_DETAILED:
                            mode_note = EXPECT_FAIL_DETAILED[n]
                            break
                want_ok = expect_ok and mode_note is None
                why = note if not expect_ok else mode_note

                for ncores in cores:
                    full = '%s -n %d' % (label, ncores)
                    checks += 1
                    limit = (args.timeout_detailed if mode == 'detailed'
                             else args.timeout)
                    ok, detail = run_config(names, ncores, workdir, workload,
                                            limit, mode)
                    if ok == want_ok:
                        status = 'PASS' if want_ok else 'PASS (known: %s)' % why
                    elif want_ok:
                        status = 'FAIL: ' + detail
                        failures.append('%s [%s]' % (full, mode))
                    else:
                        status = 'FAIL: now runs -- move it out of EXPECT_FAIL'
                        failures.append('%s [%s]' % (full, mode))
                    print('%-34s %s' % (full, status))
            print()
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    print()
    if unclassified:
        print('unclassified configs (add to EXPECT_PASS or EXPECT_FAIL): %s'
              % ', '.join(unclassified))
        failures.append('classification')
    if failures:
        print('%d of %d checks FAILED' % (len(failures), checks))
        return 1
    print('all %d checks passed' % checks)
    return 0


if __name__ == '__main__':
    sys.exit(main())
