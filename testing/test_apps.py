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
    the SDE recorder, and requires both its verdict and the front-end's banner;
    test/qemu-frontend does the same through QEMU (run-sniper --frontend=qemu).

Skips cleanly, with a reason, when a workload needs something absent -- an MPI
that has not been fetched, a Pin kit for pinball replay.

Usage:
  python3 testing/test_apps.py
  python3 testing/test_apps.py --only fft --only dvfs
"""

import argparse
import os
import shutil
import tempfile
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


def pa_check(out, rc):
    """--pa on a host that allows it (test/pa): the program simulates, and in
    its recorded trace the 64 pages it touched carry physical addresses --
    none missing or equal to the virtual one, and as many frames as pages."""
    if out.count('pa done') < 2:
        return False, 'the program did not finish under run-sniper and record-trace'
    m = re.search(r'^pa accesses=(\d+) frames=(\d+) unmapped=(\d+)', out, re.M)
    if not m:
        return False, 'no physical-address summary'
    n, frames, bad = map(int, m.groups())
    if n == 0 or bad or frames != 64:
        return False, '%d accesses to the buffer, %d frames, %d without a physical address' % (n, frames, bad)
    return True, '%d accesses to 64 pages, 64 physical frames' % n


def pa_refused_check(out, rc):
    """--pa without CAP_SYS_ADMIN (test/pa-refused): refused, with the reason."""
    m = re.search(r'pa-refused exit=(\d+)', out)
    if not m or m.group(1) == '0':
        return False, 'run-sniper --pa went ahead on a host that cannot read physical frames'
    if 'CAP_SYS_ADMIN' not in out:
        return False, 'refused without saying why'
    return True, 'refused (exit %s), naming CAP_SYS_ADMIN' % m.group(1)


def vma_check(out, rc):
    """record-trace --vma under SDE and DynamoRIO (test/dr-vma): both logs
    start at instruction 0, and each shows vma.c's fixed region in the
    snapshots taken while it was mapped and in none before or after -- the
    region is mapped after the first stretch of work and unmapped before the
    last, each far longer than the 10000 instructions between snapshots."""
    if out.count('vma done') != 2:
        return False, 'the program did not finish under both front ends'
    got = {}
    for fe, n, first, lo, hi in re.findall(r'^vma (\w+) snapshots=(\d+) first=(\S*) region=(\d+)\.\.(\d+)', out, re.M):
        n, lo, hi = int(n), int(lo), int(hi)
        if first != '0':
            return False, '%s: first snapshot at instruction %s, not 0' % (fe, first)
        if not (1 < lo <= hi < n):
            return False, '%s: the region is in snapshots %d..%d of %d, expected strictly inside' % (fe, lo, hi, n)
        got[fe] = '%d..%d of %d' % (lo, hi, n)
    if sorted(got) != ['dr', 'sde']:
        return False, 'no VMA summary for %s' % ', '.join(sorted({'dr', 'sde'} - set(got)))
    return True, 'region mapped in snapshots SDE %s, DynamoRIO %s' % (got['sde'], got['dr'])


def qemu_frontend_check(out, rc):
    """The FFT ran under the QEMU front end and passed its own check; the
    front-end line is required for the same reason as dr_frontend_check's."""
    if '[RECORD-TRACE] Using the QEMU frontend' not in out:
        return False, 'record-trace did not select the QEMU frontend'
    ok, why = fft_self_check(out, rc)
    return ok, (why + ' under QEMU') if ok else why


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


def simulated_instructions(outdir):
    """Instructions the run simulated, summed over every sim.stats under its
    output directory (a case's Makefile may run the simulator more than once,
    or in a subdirectory)."""
    total = 0
    for root, dirs, files in os.walk(outdir):
        if 'sim.stats' in files:
            for line in open(os.path.join(root, 'sim.stats')):
                if line.startswith('performance_model.instruction_count = '):
                    total += sum(int(float(v)) for v in line.split('=', 1)[1].split(',') if v.strip())
    return total


def run_stats(outdir):
    """The run's sim.stats as {key: [per-core values]} (the first found)."""
    for root, dirs, files in os.walk(outdir):
        if 'sim.stats' in files:
            st = {}
            for line in open(os.path.join(root, 'sim.stats')):
                if ' = ' in line:
                    k, v = line.split(' = ', 1)
                    st[k.strip()] = [x.strip() for x in v.split(',') if x.strip()]
            return st, root
    return {}, None


def fft_and(feature):
    """The FFT self-test, and evidence in the run's output that the variant's
    own feature did its part: each of these variants used to pass on the FFT
    self-test alone, which says nothing about hetero cores, DVFS, markers,
    the scheduler or the visualisation."""
    def check(out, rc, outdir):
        ok, why = fft_self_check(out, rc)
        if not ok:
            return ok, why
        ok2, why2 = feature(out, outdir)
        return ok2, '%s; %s' % (why, why2)
    check.needs_outdir = True
    return check


def corefreq_is(want_ghz):
    def feature(out, outdir):
        st, _ = run_stats(outdir)
        got = [round(float(v) / 1e9, 2) for v in st.get('corefreq', [])]
        if got != want_ghz:
            return False, 'core frequencies %s GHz, expected %s' % (got, want_ghz)
        return True, 'core frequencies %s GHz' % ', '.join('%g' % g for g in got)
    return feature


def dvfs_feature(out, outdir):
    """Core 1 drops to 1 GHz 1 us into the ROI (scripts/dvfs.py).  corefreq and
    cycle_count are computed from the configured frequency, so the change shows
    in time: core 0 waits at the barriers for the slow core (60% of the run
    measured, against 6% for both cores without the change)."""
    if 'Setting frequency for core 1 in DVFS domain 1 to 1000 MHz' not in out:
        return False, 'the frequency change of core 1 was not applied'
    st, _ = run_stats(outdir)
    idle = [float(v) for v in st.get('performance_model.idle_elapsed_time', [])]
    total = [float(v) for v in st.get('performance_model.elapsed_time', [])]
    if len(idle) != 2 or not total or idle[0] < 0.4 * total[0] or idle[1] > 0.2 * total[1]:
        return False, 'idle time per core %s of %s: core 0 does not wait for the slowed core 1' % (idle, total[:1])
    return True, 'core 1 at 1 GHz: core 0 idle %.0f%% of the run, core 1 %.0f%%' % (100 * idle[0] / total[0], 100 * idle[1] / total[1])


def hetero_cfg_feature(out, outdir):
    st, root = run_stats(outdir)
    cfg = open(os.path.join(root, 'sim.cfg')).read() if root else ''
    if 'dispatch_width[] = 1,2,4,1' not in cfg:
        return False, 'the per-core configurations (tiny, small, big, tiny) did not reach sim.cfg as dispatch widths 1,2,4,1'
    return True, 'per-core dispatch widths 1,2,4,1'


def power_feature(out, outdir):
    """run-sniper --power: McPAT ran and wrote an estimate (power.txt with the
    processor's runtime power, power.xml describing an x86 core for this x86
    configuration); McPAT's failures used to leave no files and pass."""
    txt, xml = os.path.join(outdir, 'power.txt'), os.path.join(outdir, 'power.xml')
    if not os.path.isfile(txt) or not os.path.isfile(xml):
        return False, 'no power.txt / power.xml: McPAT did not run or failed'
    t = open(txt).read()
    m = re.search(r'^Processor:.*?Runtime Dynamic = ([0-9.]+) W', t, re.S | re.M)
    if not m or float(m.group(1)) <= 0:
        return False, 'power.txt has no processor runtime power'
    if 'name="x86" value="1"' not in open(xml).read():
        return False, 'power.xml does not describe the x86 core as x86'
    node = re.search(r'Technology (\d+) nm', t)
    return True, 'McPAT: %s W runtime dynamic at %s nm' % (m.group(1), node.group(1) if node else '?')


def markers_feature(out, outdir):
    n0, n1 = out.count('Magic marker from thread 0'), out.count('Magic marker from thread 1')
    if not n0 or not n1 or 'str = begin' not in out:
        return False, 'markers from thread 0: %d, thread 1: %d, the begin string %s' % (n0, n1, 'seen' if 'str = begin' in out else 'missing')
    return True, '%d and %d markers from the two threads' % (n0, n1)


def scheduler_feature(out, outdir):
    st, _ = run_stats(outdir)
    by_core = [[int(float(v)) for v in st.get('thread.instructions_by_core[%d]' % c, [])] for c in range(4)]
    moved = sum(1 for t in range(len(by_core[0]) if by_core and by_core[0] else 0)
                if sum(1 for c in range(4) if by_core[c][t] > 0) > 1)
    if not moved:
        return False, 'no thread ran on more than one core: the big/small scheduler did not move threads'
    return True, '%d threads ran on more than one core' % moved


def aso_feature(out, outdir):
    for root, dirs, files in os.walk(outdir):
        if root.endswith(os.path.join('levels', 'functionbased')):
            need = ('functionbased.html', 'iptstats.json')
            if all(f in files and os.path.getsize(os.path.join(root, f)) > 0 for f in need):
                return True, 'the function-based visualisation written'
    return False, 'no viz/levels/functionbased with functionbased.html and iptstats.json'


# The checks below look at what the program printed or how it exited, which a
# native run does too; so each also requires that instructions were simulated.
# A run whose recorder failed before streaming (test/sniper-in-sniper until
# 9.0) printed the same lines and passed.
def prints(needle, note):
    def check(out, rc, needle=needle, note=note):
        return (True, note) if needle in out else (False, 'missing %r' % needle)
    check.needs_simulation = True
    return check


def exits_zero(out, rc):
    return (rc == 0, 'exit %s' % rc)
exits_zero.needs_simulation = True


# Checks that cannot pass yet, with the reason.  Recorded rather than dropped,
# on the same principle as EXPECT_FAIL in test_configs.py: a test that starts
# working should be reported, not silently absorbed.
EXPECT_FAIL = {
}

# Cases that are known not to work and are skipped unless named with --only:
# each would otherwise run to its timeout on every pass.
KNOWN_BROKEN = {
}


def sniper_in_sniper_check(out, rc, outdir):
    """Both simulations end and both report.  Until 9.0 the inner simulator
    never started (it loaded the SDE kit's libxed) and the case passed in 13 s
    with nothing simulated; then the outer deadlocked at the inner's exit (a
    thread waiting for a core its blocked sibling held, and a second
    exit_group that aborted the first); and the outer recorded no region of
    interest, the inner's SimRoiStart having been lost, so it wrote no
    statistics."""
    if rc != 0:
        return False, 'exit %s' % rc
    outer = os.path.join(outdir, 'simulation', 'sim.stats')
    inner = os.path.join(outdir, 'sim-inside', 'simulation', 'sim.stats')
    counts = []
    for name, path in (('outer', outer), ('inner', inner)):
        n = 0
        if os.path.exists(path):
            for line in open(path):
                # core.instructions: the outer fast-forwards its region, and in
                # fast-forward only the core counts (the timing model's count stays 0)
                if line.startswith('core.instructions = '):
                    n = sum(int(float(v)) for v in line.split('=', 1)[1].split(',') if v.strip())
        if not n:
            return False, 'the %s simulation reported no instructions (%s)' % (name, path)
        counts.append(n)
    return True, 'outer simulated %d instructions of the inner simulator, which simulated %d' % tuple(counts)
sniper_in_sniper_check.needs_simulation = True
sniper_in_sniper_check.needs_outdir = True


# name -> (check, why it might be skipped)
CASES = {
    'fft':             (fft_self_check, None),
    'aso':             (fft_and(aso_feature), None),
    'demo-scheduling': (fft_and(scheduler_feature), None),
    'fft-dvfs':        (fft_and(dvfs_feature), None),
    'fft-hetero':      (fft_and(corefreq_is([1.0, 2.66, 2.66, 2.0])), None),
    'fft-hetero-cfg':  (fft_and(hetero_cfg_feature), None),
    'fft-marker':      (fft_and(markers_feature), None),
    'fft-power':       (fft_and(power_feature), 'mcpat'),
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
    'qemu-frontend':   (qemu_frontend_check, 'qemu'),
    'dr-vma':          (vma_check, 'dynamorio'),
    'pa':              (pa_check, 'pa'),
    'pa-refused':      (pa_refused_check, 'no-pa'),
    'qemu-smc-rewrite': (smc_rewrite_check, 'qemu'),
    'fork':            (prints('Hello world', 'forked child ran'), None),
    'spinloop':        (prints('done', 'threads finished'), None),
    'true':            (exits_zero, None),
    'sniper-in-sniper': (sniper_in_sniper_check, None),
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
    if reason in ('pa', 'no-pa'):
        sys.path.insert(0, os.path.join(SNIPER_ROOT, 'tools'))
        import run_sniper
        if reason == 'pa' and not run_sniper.va2pa_valid():
            return 'needs CAP_SYS_ADMIN to read physical frames (pa-refused checks the refusal)'
        if reason == 'no-pa' and run_sniper.va2pa_valid():
            return 'this host can read physical frames (pa covers it)'
    if reason == 'qemu':
        build = os.environ.get('SNIPER_BUILD_DIR', os.path.join(SNIPER_ROOT, 'build', 'default'))
        if not os.path.isfile(os.path.join(build, 'lib', 'libsniper-qemu.so')):
            return 'no QEMU frontend in %s (make QEMU=ON)' % build
        qemu = os.environ.get('SNIPER_QEMU_X86_64') or os.path.join(SNIPER_ROOT, 'qemu', 'build', 'qemu-x86_64')
        if not os.path.isfile(qemu):
            return 'no QEMU (deps/sniper-deps --qemu fetch, or set SNIPER_QEMU_X86_64)'
    if reason == 'mcpat':
        mcpat = os.path.join(SNIPER_ROOT, 'mcpat')
        if not any(os.path.isfile(os.path.join(mcpat, b)) for b in ('mcpat-1.0', 'mcpat-1.0.cache')):
            return 'no McPAT (make MCPAT=ON, the default)'
    if reason == 'dynamorio':
        build = os.environ.get('SNIPER_BUILD_DIR', os.path.join(SNIPER_ROOT, 'build', 'default'))
        if not os.path.isfile(os.path.join(build, 'lib', 'libdr-frontend.so')):
            return 'no DynamoRIO frontend in %s (make DYNAMORIO=ON)' % build
        if not os.path.isfile(os.path.join(SNIPER_ROOT, 'dynamorio', 'build', 'bin64', 'drrun')):
            return 'no DynamoRIO (deps/sniper-deps --dynamorio fetch)'
    return None


def run(name, timeout, outdir):
    # Each test's run-sniper output goes to a directory of its own
    # (SNIPER_OUTPUT_DIR, honoured by run-sniper and dumpstats without -d),
    # not into test/<name>/ where the next run, or another suite, finds it.
    env = dict(os.environ, SNIPER_ROOT=SNIPER_ROOT, SNIPER_OUTPUT_DIR=outdir)
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
    ap.add_argument('--keep', action='store_true',
                    help='keep the run directories (printed at the end) instead of removing them')
    add_failfast(ap)
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(SNIPER_ROOT, 'lib', 'sniper')):
        print('error: lib/sniper not built; run make first', file=sys.stderr)
        return 2

    names = args.only or sorted(CASES)
    failures = skipped = 0
    log = RunLog('apps')
    stopped_early = None
    kept = []
    for name in names:
        check, needs = CASES[name]
        why = unavailable(needs) if needs else None
        if not why and name in KNOWN_BROKEN and not args.only:
            why = 'known broken: ' + KNOWN_BROKEN[name]
        if why:
            print('%-18s SKIP (%s)' % (name, why))
            skipped += 1
            continue

        print('%-18s ' % name, end='')
        sys.stdout.flush()
        outdir = tempfile.mkdtemp(prefix='sniper-apps-%s-' % name)
        rc, out, took = run(name, args.timeout, outdir)
        if rc is not None:
            ok, detail = check(out, rc, outdir) if getattr(check, 'needs_outdir', False) else check(out, rc)
            if ok and getattr(check, 'needs_simulation', False):
                n = simulated_instructions(outdir)
                if n == 0:
                    ok, detail = False, detail + ', but no instructions were simulated'
                else:
                    detail += '; %d instructions simulated' % n
        # A failed case keeps its directory, whatever --keep says: an
        # intermittent failure leaves its evidence instead of a bare verdict.
        if args.keep or rc is None or not ok:
            kept.append(outdir)
        else:
            shutil.rmtree(outdir, ignore_errors=True)
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
    for d in kept:
        print('run directory kept: %s' % d)
    print('all %d checks passed (%d skipped)' % (ran, skipped))
    return 0


if __name__ == '__main__':
    sys.exit(main())
