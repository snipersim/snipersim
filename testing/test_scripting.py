#!/usr/bin/env python3
"""
Regression tests for Sniper's embedded Python scripting hooks.

Runs a multi-threaded workload under each hook script and checks that the
simulation completes, the application produces the right answer, and no Python
exception escaped.  Threads matter here: the interpreter Sniper embeds today has
them, so hook callbacks cross the GIL from simulator threads.  A missing
PyGILState_Ensure() on any of those paths shows up as a segfault, and only under
load -- inspection will not find it.

Usage:
  python3 testing/test_scripting.py                 # all cases
  python3 testing/test_scripting.py --case statscb  # one case
  python3 testing/test_scripting.py --repeat 5      # shake out races
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
APP_SRC = os.path.join(HERE, 'apps', 'threaded.c')

# The workload prints this once it has joined every thread.  Any corruption of
# simulated state shows up as a different checksum or no line at all.
EXPECTED_APP_OUTPUT = re.compile(r'threads=4 total=37\.589880')

# Cores must exceed the thread count, and the static scheduler refuses to place
# more threads than it has free cores, so pin instead.
SNIPER_ARGS = [
    '-n', '8',
    '-caddress_translation_schemes/baseline',
    '-g', '--scheduler/type=pinned',
]

CASES = {
    # name            -s argument                    extra assertion
    'statscb':        ('statscb',                    r'\[STATSCB\] statsCallback invocations: [1-9]'),
    'periodic-stats': ('periodic-stats:1000:2000',   None),
    'ipcthreadtrace': ('ipcthreadtrace',             None),
    'markers':        ('markers',                    None),
    'no-script':      (None,                         None),
    # The static scheduler needs a free core per thread, so it is the one
    # scheduler that notices when -n is quietly overridden: a shared config
    # include setting total_cores = 1 once turned this -n 8 run into a one-core
    # run, and pinned hid it by time-slicing.  Kept as a canary.
    'static-scheduler': ('statscb',                  r'\[STATSCB\] statsCallback invocations: [1-9]',
                         ['-g', '--scheduler/type=static']),
}


def build_app(workdir):
    exe = os.path.join(workdir, 'threaded')
    cc = os.environ.get('CC', 'gcc')
    subprocess.run([cc, '-O1', '-pthread', '-o', exe, APP_SRC], check=True)
    return exe


def run_case(name, exe, workdir, timeout):
    script, extra_re = CASES[name][:2]
    extra_args = CASES[name][2] if len(CASES[name]) > 2 else []
    outdir = os.path.join(workdir, 'out-' + name)
    if os.path.isdir(outdir):
        shutil.rmtree(outdir)

    cmd = [os.path.join(SNIPER_ROOT, 'run-sniper')] + SNIPER_ARGS + extra_args
    if script:
        cmd += ['-s', script]
    cmd += ['-d', outdir, '--', exe]

    env = dict(os.environ)
    # Hook scripts live in $SNIPER_ROOT/scripts; test-only fixtures live here.
    env['PYTHONPATH'] = os.pathsep.join(
        [os.path.join(HERE, 'scripts'), env.get('PYTHONPATH', '')])

    try:
        p = subprocess.run(cmd, cwd=os.path.join(HERE, 'scripts'),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout, env=env, text=True)
    except subprocess.TimeoutExpired:
        return False, 'timed out after %ds (deadlock?)' % timeout

    out = p.stdout
    if p.returncode != 0:
        detail = 'exit %d' % p.returncode
        if p.returncode == -11 or p.returncode == 139:
            detail += ' (SIGSEGV)'
        elif p.returncode == -6 or p.returncode == 134:
            detail += ' (SIGABRT)'
        return False, detail
    if not EXPECTED_APP_OUTPUT.search(out):
        return False, 'application checksum missing or wrong'
    if 'Traceback (most recent call last)' in out:
        return False, 'Python exception escaped a hook'
    if extra_re and not re.search(extra_re, out):
        return False, 'expected output %r not seen' % extra_re
    return True, 'ok'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--case', action='append', choices=sorted(CASES),
                    help='run only this case (repeatable)')
    ap.add_argument('--repeat', type=int, default=1)
    ap.add_argument('--timeout', type=int, default=600)
    ap.add_argument('--keep', action='store_true', help='keep the work directory')
    args = ap.parse_args()

    if not os.path.exists(os.path.join(SNIPER_ROOT, 'lib', 'sniper')):
        print('error: lib/sniper not built; run make first', file=sys.stderr)
        return 2

    cases = args.case or sorted(CASES)
    workdir = tempfile.mkdtemp(prefix='sniper-scripting-')
    failures = []
    try:
        exe = build_app(workdir)
        for name in cases:
            for it in range(args.repeat):
                label = name if args.repeat == 1 else '%s [%d/%d]' % (name, it + 1, args.repeat)
                sys.stdout.write('%-28s ' % label)
                sys.stdout.flush()
                ok, msg = run_case(name, exe, workdir, args.timeout)
                print('PASS' if ok else 'FAIL: ' + msg)
                if not ok:
                    failures.append((label, msg))
    finally:
        if args.keep:
            print('work directory kept at %s' % workdir)
        else:
            shutil.rmtree(workdir, ignore_errors=True)

    print()
    if failures:
        print('%d of %d checks FAILED' % (len(failures), len(cases) * args.repeat))
        return 1
    print('all %d checks passed' % (len(cases) * args.repeat))
    return 0


if __name__ == '__main__':
    sys.exit(main())
