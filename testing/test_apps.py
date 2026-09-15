#!/usr/bin/env python3
"""
Regression test: the workloads in test/ produce correct results, not just exit 0.

Every directory under test/ has a `make run` target, and until now nothing ran
them.  Wiring them up on its own would not have helped much: `make run` reports
whether the simulator crashed, and almost nothing in there checked what came
out.  A configuration that silently mis-simulated a program would have passed.

So each entry below carries a check of its own, and each check is grounded in
something the workload already reports rather than a number invented here:

  - the SPLASH-2 FFT has always been able to verify itself.  It checksums the
    input, transforms and inverse-transforms it, and compares.  Nothing ever
    passed the -t flag that turns this on, so nine directories were running a
    self-checking workload with its self-check disabled.
  - test/api asks the simulator whether it is being simulated, and prints
    different text when it is not.
  - test/dvfs sets a frequency and reads it back.
  - test/mpi computes pi.
  - test/smc rewrites its own code and prints the result.

Skips cleanly, with a reason, when a workload needs something absent -- an MPI
that has not been fetched, a Pin kit for pinball replay.

Usage:
  python3 testing/test_apps.py
  python3 testing/test_apps.py --only fft --only dvfs
"""

import argparse
import os
import re
import signal
import subprocess
import sys
import time

SNIPER_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_DIR = os.path.join(SNIPER_ROOT, 'test')


def fft_self_check(out, rc):
    """The workload's own inverse-FFT comparison."""
    if 'TEST FAILED' in out:
        m = re.search(r'Checksum difference is (\S+)', out)
        return False, 'the workload reports TEST FAILED (%s)' % (m.group(1) if m else '?')
    n = out.count('TEST PASSED')
    if not n:
        return False, 'the workload printed no verdict; was -t dropped?'
    return True, '%d x TEST PASSED' % n


def api_in_simulator(out, rc):
    if 'API Test: Not running in the simulator' in out:
        return False, 'SimInSimulator() returned false while simulated'
    if 'API Test: Running in the simulator' not in out:
        return False, 'the workload printed no verdict'
    return True, 'SimInSimulator() true'


def dvfs_readback(out, rc):
    """A frequency that is set must read back as the value that was set."""
    wanted = [int(m) for m in re.findall(r'Setting frequency to (\d+) MHz', out)]
    got = [int(m) for m in re.findall(r'Current Freq = (\d+) MHz', out)]
    if not wanted:
        return False, 'the workload set no frequency'
    if len(got) < 2:
        return False, 'the workload read back no frequency'
    # The first readback precedes any change; each later one follows a set.
    for i, w in enumerate(wanted):
        if i + 1 < len(got) and got[i + 1] != w:
            return False, 'set %d MHz but read back %d MHz' % (w, got[i + 1])
    return True, 'set and read back %s MHz' % ', '.join(str(w) for w in wanted)


def mpi_pi(out, rc):
    m = re.search(r'pi is approximately (\d+\.\d+)', out)
    if not m:
        return False, 'the workload printed no result'
    import math
    err = abs(float(m.group(1)) - math.pi)
    if err > 1e-9:
        return False, 'pi off by %.3g' % err
    return True, 'pi correct to %.1e' % err


def smc_result(out, rc):
    """Self-modifying code: the rewritten instruction must be the one that ran."""
    if 'Good morning!' not in out:
        return False, 'the rewritten code did not run (no "Good morning!")'
    return True, 'rewritten code executed'


def prints(needle, note):
    def check(out, rc, needle=needle, note=note):
        return (True, note) if needle in out else (False, 'missing %r' % needle)
    return check


def exits_zero(out, rc):
    return (rc == 0, 'exit %s' % rc)


# Checks that cannot pass yet, with the reason.  Recorded rather than dropped,
# on the same principle as EXPECT_FAIL in test_configs.py: a test that starts
# working should be reported, not silently absorbed.
EXPECT_FAIL = {
    'signal': 'signal delivery to the application is not implemented -- '
              'general/enable_signals is declared in config/base.cfg and named '
              'in the CHANGELOG, but no code anywhere reads it, so the handler '
              'never runs and the process dies at the NULL dereference',
}


# name -> (check, why it might be skipped)
CASES = {
    'fft':             (fft_self_check, None),
    'aso':             (fft_self_check, None),
    'demo-scheduling': (fft_self_check, None),
    'fft-dvfs':        (fft_self_check, None),
    'fft-hetero':      (fft_self_check, None),
    'fft-hetero-cfg':  (fft_self_check, None),
    'fft-marker':      (fft_self_check, None),
    'fft-nn':          (fft_self_check, None),
    'pinplay':         (fft_self_check, None),
    'api':             (api_in_simulator, None),
    'dvfs':            (dvfs_readback, None),
    'mpi':             (mpi_pi, 'mpich'),
    'mpi-omp':         (prints('Hybrid', 'hybrid run completed'), 'mpich'),
    'smc':             (smc_result, None),
    'fork':            (prints('Hello world', 'forked child ran'), None),
    'signal':          (prints('Received signal', 'signal delivered'), None),
    'spinloop':        (prints('done', 'threads finished'), None),
    'true':            (exits_zero, None),
    'sniper-in-sniper': (exits_zero, None),
}


def unavailable(reason):
    if reason == 'mpich':
        if not os.path.isfile(os.path.join(SNIPER_ROOT, 'mpich', 'bin', 'mpiexec')):
            return 'no MPI (deps/sniper-deps --mpi fetch)'
    return None


def run(name, timeout):
    env = dict(os.environ, SNIPER_ROOT=SNIPER_ROOT)
    env.setdefault('SNIPER_BUILD_DIR', os.path.join(SNIPER_ROOT, 'build', 'default'))
    t0 = time.time()
    with subprocess.Popen(['make', 'run'], cwd=os.path.join(TEST_DIR, name),
                          env=env, start_new_session=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True) as p:
        try:
            out, _ = p.communicate(timeout=timeout)
            return p.returncode, out, time.time() - t0
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                p.kill()
            out, _ = p.communicate()
            return None, out or '', time.time() - t0


def main():
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass

    ap = argparse.ArgumentParser()
    ap.add_argument('--only', action='append', choices=sorted(CASES))
    ap.add_argument('--timeout', type=int, default=900)
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(SNIPER_ROOT, 'lib', 'sniper')):
        print('error: lib/sniper not built; run make first', file=sys.stderr)
        return 2

    names = args.only or sorted(CASES)
    failures = skipped = 0
    for name in names:
        check, needs = CASES[name]
        why = unavailable(needs) if needs else None
        if why:
            print('%-18s SKIP (%s)' % (name, why))
            skipped += 1
            continue

        print('%-18s ' % name, end='')
        sys.stdout.flush()
        rc, out, took = run(name, args.timeout)
        if rc is None:
            print('FAIL: timed out after %ds' % args.timeout)
            failures += 1
            continue
        ok, detail = check(out, rc)
        known = EXPECT_FAIL.get(name)
        if known and not ok:
            print('PASS (known: %s) [%ds]' % (known, round(took)))
        elif known and ok:
            print('FAIL: now works -- remove it from EXPECT_FAIL [%ds]' % round(took))
            failures += 1
        else:
            print('%s (%s) [%ds]' % ('PASS' if ok else 'FAIL', detail, round(took)))
            if not ok:
                failures += 1

    print()
    ran = len(names) - skipped
    if failures:
        print('%d of %d checks FAILED (%d skipped)' % (failures, ran, skipped))
        return 1
    print('all %d checks passed (%d skipped)' % (ran, skipped))
    return 0


if __name__ == '__main__':
    sys.exit(main())
