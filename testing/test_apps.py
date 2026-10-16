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
  - test/fft-nn selects the libtorch branch predictor and is skipped when the
    build has no libtorch (TORCH=ON).
  - test/dr-frontend runs the FFT through the DynamoRIO front end instead of
    the SDE recorder, and requires both its verdict and the front-end's banner.

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

from common.failfast import FailFast, add_argument as add_failfast
from common.runlog import RunLog

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


def pinplay_check(out, rc):
    """The replay ran to completion, and the pinball's recorded run passed
    the FFT's own check.  A replay does not reproduce the application's
    output (its writes were logged, not re-executed), so the verdict comes
    from the recording: test/pinplay keeps it in record.log.  Before this,
    the case passed only when the pinball happened to be re-recorded in the
    same run, and failed on every replay of an existing one."""
    m = re.search(r'Finished replaying thread .*?, (\d+) instructions', out)
    if not m:
        return False, 'the replayer did not report a completed replay'
    ok, why = fft_self_check(out, rc)
    if not ok:
        return False, why + ' (recorded run)'
    return True, 'replayed %s instructions; recorded run: %s' % (m.group(1), why)


def dr_frontend_check(out, rc):
    """The FFT ran under the DynamoRIO front end and passed its own check.
    The front-end line is required so that a run that silently fell back to
    the SDE recorder cannot pass as a DynamoRIO run."""
    if '[RECORD-TRACE] Using DynamoRIO frontend' not in out:
        return False, 'record-trace did not select the DynamoRIO frontend'
    ok, why = fft_self_check(out, rc)
    return ok, (why + ' under DynamoRIO') if ok else why


def dr_threads_check(out, rc):
    """A threaded program under the DynamoRIO front end: every worker was
    traced (the front end used to abort on the first thread glibc created
    with clone3) and the total is right, which needs every pthread_join to
    have waited for its own worker."""
    if '[RECORD-TRACE] Using DynamoRIO frontend' not in out:
        return False, 'record-trace did not select the DynamoRIO frontend'
    traced = len(set(re.findall(r'\[TRACE:(\d+)\] -- DONE --', out)))
    if traced < 5:
        return False, 'only %d of 5 threads traced' % traced
    m = re.search(r'threads=4 total=([0-9.]+)', out)
    if not m:
        return False, 'the workload printed no total'
    if m.group(1) != '37.589880':
        return False, 'wrong total %s (want 37.589880)' % m.group(1)
    return True, 'total=%s, %d threads traced, under DynamoRIO' % (m.group(1), traced)


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


def hybrid_sum(out, rc):
    """MPI + OpenMP: every rank's slabs, summed by MPI_Reduce on rank 0.

    process_slab() returns the same value for each of the 16 slabs, so the total
    is fixed regardless of how the ranks and threads divide them up -- which is
    the point: it checks that every slab was processed exactly once, across both
    levels of parallelism.  The check used to look for a word the workload never
    printed, so it could only ever fail.
    """
    m = re.search(r'Sum is (\S+)', out)
    if not m:
        return False, 'the workload printed no sum'
    got = float(m.group(1))
    want = 167736.67102662913      # 16 slabs x 10483.54193916432
    # The workload prints with %lg, so what arrives here is rounded to six
    # significant digits: 167737.  The tolerance has to clear that rounding
    # (~3e-6 relative here) without being loose enough to miss a lost slab,
    # which would move the total by 1/16th.
    if abs(got - want) > 1e-5 * want:
        return False, 'sum is %g, expected %g' % (got, want)
    return True, 'sum %g over 16 slabs' % got


def smc_result(out, rc):
    """Self-modifying code: the rewritten instruction must be the one that ran."""
    if 'Good morning!' not in out:
        return False, 'the rewritten code did not run (no "Good morning!")'
    return True, 'rewritten code executed'


def smc_rewrite_check(out, rc):
    """Code rewritten in place (test/smc-rewrite): the simulation completes
    with every run returning its own value, and the trace carries each run's
    own bytes at the rewritten address -- A, then B (a different first
    instruction, of a different length), then A again.  Before code changes
    were tracked, the trace repeated A's bytes for all three."""
    for tag, want in (('A first', 1), ('B rewritten', 2), ('A again', 1)):
        m = re.search(r'%s pc=0x[0-9a-f]+ returned=(-?\d+)' % tag, out)
        if not m:
            return False, 'no %r line from the simulated run' % tag
        if int(m.group(1)) != want:
            return False, '%s returned %s, expected %d' % (tag, m.group(1), want)
    traced = re.findall(r'^traced [0-9a-f]+ ((?:[0-9a-f]{2} ?)+)', out, re.M)
    firsts = [t.split()[:2] for t in traced]
    want = [['b8', '01'], ['31', 'c0'], ['b8', '01']]
    if firsts != want:
        return False, 'traced bytes at the rewritten address %s, expected %s' % (
            [' '.join(f) for f in firsts], [' '.join(w) for w in want])
    return True, 'simulated; traced A, B, A at one address: %s' % ', '.join(' '.join(f) for f in firsts)


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
    'fft-nn':          (fft_self_check, 'torch'),
    'pinplay':         (pinplay_check, None),
    'dr-frontend':     (dr_frontend_check, 'dynamorio'),
    'dr-threads':      (dr_threads_check, 'dynamorio'),
    'api':             (api_in_simulator, None),
    'dvfs':            (dvfs_readback, None),
    'mpi':             (mpi_pi, 'mpich'),
    'mpi-omp':         (hybrid_sum, 'mpich'),
    'smc':             (smc_result, None),
    'smc-rewrite':     (smc_rewrite_check, None),
    'dr-smc-rewrite':  (smc_rewrite_check, 'dynamorio'),
    'fork':            (prints('Hello world', 'forked child ran'), None),
    'spinloop':        (prints('done', 'threads finished'), None),
    'true':            (exits_zero, None),
    'sniper-in-sniper': (exits_zero, None),
}


def unavailable(reason):
    if reason == 'mpich':
        if not os.path.isfile(os.path.join(SNIPER_ROOT, 'mpich', 'bin', 'mpiexec')):
            return 'no MPI (deps/sniper-deps --mpi fetch)'
    if reason == 'torch':
        # The nn branch predictor needs libtorch, which the build carries only
        # with TORCH=ON; the build's own config says whether it does.
        build = os.environ.get('SNIPER_BUILD_DIR', os.path.join(SNIPER_ROOT, 'build', 'default'))
        try:
            cfg = open(os.path.join(build, 'config', 'sniper.py')).read()
        except OSError:
            cfg = ''
        if re.search(r'^torch_home=""', cfg, re.M) or 'torch_home=' not in cfg:
            return 'no libtorch in %s (make TORCH=ON)' % build
    if reason == 'dynamorio':
        build = os.environ.get('SNIPER_BUILD_DIR', os.path.join(SNIPER_ROOT, 'build', 'default'))
        if not os.path.isfile(os.path.join(build, 'lib', 'libdr-frontend.so')):
            return 'no DynamoRIO frontend in %s (make DYNAMORIO=ON)' % build
        if not os.path.isfile(os.path.join(SNIPER_ROOT, 'dynamorio', 'build', 'bin64', 'drrun')):
            return 'no DynamoRIO (deps/sniper-deps --dynamorio fetch)'
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
    add_failfast(ap)
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(SNIPER_ROOT, 'lib', 'sniper')):
        print('error: lib/sniper not built; run make first', file=sys.stderr)
        return 2

    names = args.only or sorted(CASES)
    failures = skipped = 0
    log = RunLog('apps')
    stopped_early = None
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
            # A timeout is the most expensive check there is; record it, or the
            # suite total quietly omits the worst case.
            log.record(name, took, 'TIMEOUT')
            print('FAIL: timed out after %ds' % args.timeout)
            failures += 1
            if args.failfast:
                stopped_early = name
                break
            continue
        ok, detail = check(out, rc)
        known = EXPECT_FAIL.get(name)
        if known and not ok:
            log.record(name, took, 'PASS')
            print('PASS (known: %s) [%ds]' % (known, round(took)))
        elif known and ok:
            log.record(name, took, 'FAIL')
            print('FAIL: now works -- remove it from EXPECT_FAIL [%ds]' % round(took))
            failures += 1
        else:
            log.record(name, took, 'PASS' if ok else 'FAIL')
            print('%s (%s) [%ds]' % ('PASS' if ok else 'FAIL', detail, round(took)))
            if not ok:
                failures += 1
        if args.failfast and failures:
            stopped_early = name
            break

    if stopped_early:
        # Each case runs in its own test/<name>, which is where the output and
        # the logs already are; nothing to keep, only somewhere to point.
        print('\nstopped at the first failure; %s ran in %s'
              % (stopped_early, os.path.join(SNIPER_ROOT, 'test', stopped_early)))
    log.finish()
    print()
    # len(names) would be wrong after --failfast: the checks it never reached
    # are not checks that ran.
    ran = len(log.checks)
    if failures:
        print('%d of %d checks FAILED (%d skipped)' % (failures, ran, skipped))
        return 1
    print('all %d checks passed (%d skipped)' % (ran, skipped))
    return 0


if __name__ == '__main__':
    sys.exit(main())
