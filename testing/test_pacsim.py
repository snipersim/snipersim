#!/usr/bin/env python3
"""
Validation: sampled simulation is faster than full simulation, and agrees with it.

Pac-Sim (--pacsim, --dynamic-sampling) simulates part of a run in detail and
extrapolates the rest.  That is only worth having if both halves of the bargain
hold, so both are measured here against the same workload:

  speedup   the sampled run finishes in less wall-clock time than the full
            detailed run.  A sampling methodology that is not faster has
            nothing to offer, and fixed overheads can make one slower on a
            small enough input -- which is why the workload is sized rather
            than minimal.

  accuracy  the estimate the sampled run reports is close to the answer the
            full detailed run computes.  This is the number that decides
            whether results from a sampled run can be believed.

Both are reported, not just asserted, because the figures are the point: a
change that keeps the speedup above 1.0 while halving it is a regression the
pass/fail line alone would hide.

The workload (testing/apps/phased.c) alternates memory-bound and compute-bound
phases across barriers.  That shape matters -- see the comment at the top of
that file.  Against a uniform workload any sampler is accurate and this test
would prove nothing.

Both runs are ROI-scoped to the phase loop.  Building the workload's chase
cycles costs more instructions than all the phases together, and none of it
has phase structure, so measuring it would bury the thing being tested.

This is a validation, not a smoke test: the baseline is a full detailed
simulation and takes minutes.  It is deliberately not part of `make regress`.

Skips cleanly, with a reason, when the recorder has no Pac-Sim support.

Usage:
  python3 testing/test_pacsim.py
  python3 testing/test_pacsim.py --phases 24 --tolerance 0.10
"""

import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile
import time

SNIPER_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORKLOAD_SRC = os.path.join(SNIPER_ROOT, 'testing', 'apps', 'phased.c')

sys.path.insert(0, os.path.join(SNIPER_ROOT, 'tools'))

# The knob record-trace looks for; kept in step with it deliberately.
PACSIM_KNOB = b'sniper:pacsim'


def build_dir():
    for name in ('build/release', 'build/default', '.'):
        if os.path.isfile(os.path.join(SNIPER_ROOT, name, 'lib', 'sniper')):
            return os.path.join(SNIPER_ROOT, name)
    if os.path.isfile(os.path.join(SNIPER_ROOT, 'lib', 'sniper')):
        return SNIPER_ROOT
    return None


def recorder_paths():
    import glob
    pats = ('sift/recorder/obj-*/sift_recorder',
            'sift/recorder/obj-*/sde_sift_recorder.so')
    out = []
    for p in pats:
        out += glob.glob(os.path.join(SNIPER_ROOT, p))
    return out


def prerequisites():
    """Return (info, None) or (None, reason-to-skip)."""
    b = build_dir()
    if not b:
        return None, 'no build directory (run make first)'

    tools = recorder_paths()
    if not tools:
        return None, 'no SIFT recorder built'

    for t in tools:
        try:
            with open(t, 'rb') as f:
                if PACSIM_KNOB in f.read():
                    return {'build': b, 'recorder': t}, None
        except IOError:
            continue

    return None, ('the SIFT recorder does not implement Pac-Sim '
                  '(no "%s" knob); see pacsimscripts/README.md'
                  % PACSIM_KNOB.decode())


def run(cmd, timeout, **kwargs):
    """Run in its own process group so a timeout takes the whole tree with it."""
    kwargs.setdefault('stdout', subprocess.PIPE)
    kwargs.setdefault('stderr', subprocess.STDOUT)
    kwargs.setdefault('text', True)
    t0 = time.time()
    with subprocess.Popen(cmd, start_new_session=True, **kwargs) as proc:
        try:
            out, _ = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                proc.kill()
            out, _ = proc.communicate()
            return None, out or '', time.time() - t0
        return proc.returncode, out or '', time.time() - t0


def build_workload(workdir, threads, phases):
    exe = os.path.join(workdir, 'phased')
    cc = os.environ.get('CC', 'gcc')
    subprocess.run([cc, '-O1',
                    '-I', os.path.join(SNIPER_ROOT, 'include'),
                    '-DNTHREADS=%d' % threads,
                    '-DNPHASES=%d' % phases,
                    '-o', exe, WORKLOAD_SRC, '-lpthread'], check=True)
    return exe


def checksum_of(out):
    m = re.search(r'phased: threads=\d+ phases=\d+ checksum=(\S+) visited=(\d+)', out)
    return m.groups() if m else None


def elapsed_seconds(resultsdir):
    """The simulated runtime the full detailed run computed, in seconds.

    performance_model.elapsed_time is per core and in femtoseconds; the
    application's runtime is the longest of them, and 1e15 fs is a second --
    the same units get_full_time() reports, so the two are comparable.
    """
    import sniper_lib
    res = sniper_lib.get_results(resultsdir=resultsdir)['results']
    per_core = res['performance_model.elapsed_time']
    return max(per_core) / 1e15


def simulate(exe, outputdir, threads, extra, timeout):
    """Both runs are ROI-scoped, and both the same way.

    Without --roi the measurement is swamped by setup: building the chase
    cycles costs more instructions than every phase combined, and it has no
    phase structure for a sampler to work with.  Confining both runs to the
    phase loop is what makes the two numbers comparable and the accuracy figure
    mean what it says.
    """
    cmd = [os.path.join(SNIPER_ROOT, 'run-sniper'), '--roi',
           '-n', str(threads), '-c', 'gainestown', '-d', outputdir] + extra + ['--', exe]
    return run(cmd, timeout)


def measure(p, workdir, args):
    """Run the workload twice -- full, then sampled -- and return both results."""
    exe = build_workload(workdir, args.threads, args.phases)

    full_dir = os.path.join(workdir, 'full')
    rc, out, wall_full = simulate(exe, full_dir, args.threads, [], args.timeout)
    if rc is None:
        raise RuntimeError('the full detailed run timed out after %ds' % args.timeout)
    if rc != 0:
        raise RuntimeError('the full detailed run failed (exit %s)' % rc)
    truth_sum = checksum_of(out)
    truth = elapsed_seconds(os.path.join(full_dir, 'simulation'))

    samp_dir = os.path.join(workdir, 'sampled')
    rc, out, wall_samp = simulate(exe, samp_dir, args.threads, ['--pacsim'], args.timeout)
    if rc is None:
        raise RuntimeError('the sampled run timed out after %ds' % args.timeout)
    if rc != 0:
        raise RuntimeError('the sampled run failed (exit %s)' % rc)
    samp_sum = checksum_of(out)

    m = re.search(r'\[PacSim\] Estimated sim time ([0-9.]+)s', out)
    if not m:
        raise RuntimeError('the sampled run reported no estimate')
    estimate = float(m.group(1))

    return {
        'truth': truth, 'estimate': estimate,
        'wall_full': wall_full, 'wall_sampled': wall_samp,
        'checksum_full': truth_sum, 'checksum_sampled': samp_sum,
    }


def case_same_program(r, args):
    """Both runs executed the same workload.

    Checked first: a speedup or an accuracy figure comparing two different
    programs would be meaningless, and nothing else here would notice.
    """
    if not r['checksum_full'] or not r['checksum_sampled']:
        return False, 'a run printed no checksum'
    if r['checksum_full'] != r['checksum_sampled']:
        return False, 'checksums differ: full %s, sampled %s' % (
            r['checksum_full'], r['checksum_sampled'])
    return True, 'checksum %s in both runs' % (r['checksum_full'][0],)


def case_faster(r, args):
    """The sampled run is faster in wall-clock time than the full run."""
    speedup = r['wall_full'] / r['wall_sampled'] if r['wall_sampled'] else 0.0
    detail = '%.2fx (%.0fs full, %.0fs sampled)' % (
        speedup, r['wall_full'], r['wall_sampled'])
    if speedup < args.min_speedup:
        return False, 'only %s, wanted at least %.2fx' % (detail, args.min_speedup)
    return True, detail


def case_accurate(r, args):
    """The estimate agrees with the full detailed answer."""
    truth, est = r['truth'], r['estimate']
    if truth <= 0:
        return False, 'the full run reported no elapsed time'
    err = abs(est - truth) / truth
    detail = '%.2f%% error (%.6gs estimated, %.6gs actual)' % (100 * err, est, truth)
    if err > args.tolerance:
        return False, '%s, tolerance %.1f%%' % (detail, 100 * args.tolerance)
    return True, detail


CASES = {
    'same-program': case_same_program,
    'faster':       case_faster,
    'accurate':     case_accurate,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--case', action='append', choices=sorted(CASES))
    ap.add_argument('--threads', type=int, default=4)
    ap.add_argument('--phases', type=int, default=16)
    ap.add_argument('--tolerance', type=float, default=0.10,
                    help='allowed relative error in the estimate (default: %(default)s)')
    ap.add_argument('--min-speedup', type=float, default=1.0,
                    help='minimum acceptable wall-clock speedup (default: %(default)s)')
    ap.add_argument('--timeout', type=int, default=3600)
    ap.add_argument('--keep', action='store_true', help='keep the working directory')
    args = ap.parse_args()

    p, reason = prerequisites()
    if not p:
        print('SKIP: %s' % reason)
        return 0

    workdir = tempfile.mkdtemp(prefix='sniper-pacsim-')
    try:
        print('Simulating the workload twice; the full detailed run is the slow one.')
        try:
            r = measure(p, workdir, args)
        except RuntimeError as e:
            print('ERROR: %s' % e, file=sys.stderr)
            return 1

        names = args.case or sorted(CASES)
        failures = 0
        for name in names:
            ok, detail = CASES[name](r, args)
            print('%-14s %s (%s)' % (name, 'PASS' if ok else 'FAIL', detail))
            failures += 0 if ok else 1

        print()
        if failures:
            print('%d of %d checks FAILED' % (failures, len(names)))
            return 1
        print('all %d checks passed' % len(names))
        return 0
    finally:
        if args.keep:
            print('working directory kept: %s' % workdir)
        else:
            import shutil
            shutil.rmtree(workdir, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
