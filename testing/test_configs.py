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

Usage:
  python3 testing/test_configs.py
  python3 testing/test_configs.py --only gainestown
  python3 testing/test_configs.py --cores 1,4,8
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

DEFAULT_CORES = (1, 4)

# Configurations that must run cleanly at every core count checked.
EXPECT_PASS = [
    'atd', 'beckton', 'cacheonly', 'default', 'dram-cache', 'dunnington',
    'dunnington16', 'gainestown', 'interval', 'jikes', 'llc-qbs', 'nehalem',
    'nehalem-lite', 'noc', 'nuca-cache', 'oneipc', 'prefetcher', 'riscv',
    'riscv-mediumboom', 'rob', 'sampling', 'silvermont', 'smt1', 'smt2',
    'smt4', 'smt8',
]

# Configurations that cannot run in a plain x86 build, with the reason.  Listed
# rather than skipped, so that one starting to work is reported too.
EXPECT_FAIL = {
    'cortex-a53': 'ARM target: needs an ARM frontend and an ARM trace '
                  '("Unknown architecture armv8")',
    'cortex-a72': 'ARM target: needs an ARM frontend and an ARM trace '
                  '("Unknown architecture armv8")',
    'kingscross': 'mesh topology is fixed at 16:4, so it needs 64 cores '
                  '("Invalid mesh size 16:4")',
}

# Not a configuration to run on its own.
EXCLUDE = {'base'}


def shipped_configs():
    return sorted(os.path.splitext(f)[0]
                  for f in os.listdir(CONFIG_DIR) if f.endswith('.cfg'))


def run_config(name, ncores, workdir, timeout):
    outdir = os.path.join(workdir, 'out-%s-n%d' % (name, ncores))
    shutil.rmtree(outdir, ignore_errors=True)
    cmd = [os.path.join(SNIPER_ROOT, 'run-sniper'),
           '-n', str(ncores), '--fast-forward', '-c' + name,
           '-d', outdir, '--', '/bin/true']
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout, text=True, cwd=SNIPER_ROOT)
    except subprocess.TimeoutExpired:
        return False, 'timed out after %ds' % timeout

    if p.returncode == 0:
        shutil.rmtree(outdir, ignore_errors=True)
        return True, 'ok'

    missing = re.search(r'Configuration value (\S+) not found', p.stdout)
    if missing:
        detail = 'missing config value %s' % missing.group(1)
    else:
        err = re.search(r'\*ERROR\*\s*(.{0,60})', p.stdout)
        detail = err.group(1).strip() if err else 'exit %d' % p.returncode
    return False, detail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', action='append', help='check only this config (repeatable)')
    ap.add_argument('--cores', default=','.join(str(c) for c in DEFAULT_CORES),
                    help='comma-separated core counts (default: %(default)s)')
    ap.add_argument('--timeout', type=int, default=300)
    args = ap.parse_args()

    if not os.path.exists(os.path.join(SNIPER_ROOT, 'lib', 'sniper')):
        print('error: lib/sniper not built; run make first', file=sys.stderr)
        return 2

    cores = [int(c) for c in args.cores.split(',')]
    classified = set(EXPECT_PASS) | set(EXPECT_FAIL) | EXCLUDE
    unclassified = [c for c in shipped_configs() if c not in classified]

    names = EXPECT_PASS + sorted(EXPECT_FAIL)
    if args.only:
        names = [n for n in names if n in args.only]

    workdir = tempfile.mkdtemp(prefix='sniper-configs-')
    failures = []
    try:
        for name in names:
            expect_ok = name in EXPECT_PASS
            for ncores in cores:
                label = '%s -n %d' % (name, ncores)
                ok, detail = run_config(name, ncores, workdir, args.timeout)
                if ok == expect_ok:
                    status = 'PASS' if expect_ok else 'PASS (known: %s)' % EXPECT_FAIL[name]
                elif expect_ok:
                    status = 'FAIL: ' + detail
                    failures.append(label)
                else:
                    status = 'FAIL: now runs -- move it out of EXPECT_FAIL'
                    failures.append(label)
                print('%-26s %s' % (label, status))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    print()
    if unclassified:
        print('unclassified configs (add to EXPECT_PASS or EXPECT_FAIL): %s'
              % ', '.join(unclassified))
        failures.append('classification')
    total = len(names) * len(cores)
    if failures:
        print('%d of %d checks FAILED' % (len(failures), total))
        return 1
    print('all %d checks passed' % total)
    return 0


if __name__ == '__main__':
    sys.exit(main())
