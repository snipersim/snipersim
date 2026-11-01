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
  - test/dr-timed-waits and test/dr-timers run test/timed-waits' and
    test/timers' guests through the DynamoRIO front end, to a file and live,
    and check the clocks start at the time zero.

Skips cleanly, with a reason, when a workload needs something absent -- an MPI
that has not been fetched, a Pin kit for pinball replay.

Usage:
  python3 testing/test_apps.py
  python3 testing/test_apps.py --only fft --only dvfs
"""

import pathlib
import io
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
TIME_START_DEFAULT = 1337000000   # Sift::TIME_START_DEFAULT, osemu/time_start's default


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
            for line in io.StringIO(pathlib.Path(os.path.join(root, 'sim.stats')).read_text()):
                if line.startswith('performance_model.instruction_count = '):
                    total += sum(int(float(v)) for v in line.split('=', 1)[1].split(',') if v.strip())
    return total


def run_stats(outdir):
    """The run's sim.stats as {key: [per-core values]} (the first found)."""
    for root, dirs, files in os.walk(outdir):
        if 'sim.stats' in files:
            st = {}
            for line in io.StringIO(pathlib.Path(os.path.join(root, 'sim.stats')).read_text()):
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


def cpistack_items_mismatch():
    """tools/cpistack_items.py's variants must place the same components: a
    run's component missing from one (DataCacheprefetch-no-mapping from the
    use_simple_mem=False stack, SyncSyscall from the simple sync item) is
    reported by cpistack as 'not in all_items' only when a run has it.  And
    each group's members must be items ('recv' was not).  None when they
    agree, else what differs."""
    sys.path.insert(0, os.path.join(SNIPER_ROOT, 'tools'))
    import cpistack_items
    def keys(items):
        out = set()
        for _name, _threshold, k in items:
            out |= keys(k) if isinstance(k, list) else set(k if isinstance(k, tuple) else (k,))
        return out
    variants = {(sync, mem): keys(cpistack_items.build_itemlist(use_simple_sync=sync, use_simple_mem=mem))
                for sync in (False, True) for mem in (True, False)}
    want = variants[(False, True)]                      # cpistack's default
    for v, got in sorted(variants.items()):
        if got != want:
            return 'cpistack_items (use_simple_sync=%s, use_simple_mem=%s) differs from the default: missing %s, extra %s' % (
                v[0], v[1], sorted(want - got), sorted(got - want))
    top = {name for name, _t, _k in cpistack_items.build_itemlist()}
    for legacy in (False, True):
        for group, _color, members in cpistack_items.build_grouplist(legacy=legacy):
            if set(members) - top:
                return 'cpistack_items group %s names %s, which are not items' % (group, sorted(set(members) - top))
        grouped = set(m for _g, _c, ms in cpistack_items.build_grouplist(legacy=legacy) for m in ms)
        if top - grouped:
            return 'cpistack_items items %s are in no group' % sorted(top - grouped)
    return None


def simout_feature(out, outdir):
    """The summary and the CPI stack (E9: nothing checked their contents).
    sim.out must give each core the instruction count sim.stats has and no
    negative idle time (a core runs up to a quantum past the last barrier,
    which sim.out showed as -155 ns); tools/cpistack.py must place every
    component the run reported (it dropped the CXL, shadow-cache, page-fault
    and fast-fault ones with a warning) and its total must be the run's CPI."""
    st, simdir = run_stats(outdir)
    if not simdir:
        return False, 'no sim.stats'
    try:
        simout = pathlib.Path(os.path.join(simdir, 'sim.out')).read_text()
    except OSError:
        return False, 'no sim.out'
    insns = [int(float(v)) for v in st.get('performance_model.instruction_count', [])]
    m = re.search(r'^\s*Instructions\s*\|(.*)$', simout, re.M)
    got = [int(v) for v in m.group(1).replace('|', ' ').split()] if m else []
    if not insns or got != insns:
        return False, 'sim.out gives %s instructions, sim.stats %s' % (got, insns)
    m = re.search(r'^\s*Idle time \(ns\)\s*\|(.*)$', simout, re.M)
    if not m or any(int(v) < 0 for v in m.group(1).replace('|', ' ').split()):
        return False, 'sim.out has a negative idle time: %s' % (m.group(0).strip() if m else 'no idle row')
    bad = cpistack_items_mismatch()
    if bad:
        return False, bad
    # Every variant of the stack, not only the default: --no-simple-mem
    # lacked DataCacheprefetch-no-mapping
    for variant in (['--no-simple-mem'], ['--simplified']):
        p = subprocess.run([sys.executable, os.path.join(SNIPER_ROOT, 'tools', 'cpistack.py'), '--cpi', '-d', simdir,
                            '-o', os.path.join(outdir, 'cpi-stack' + variant[0])] + variant,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if 'not in all_items' in p.stdout or p.returncode != 0:
            return False, 'cpistack %s: %s' % (variant[0], (re.search(r'Also found.*', p.stdout) or
                                                           re.search(r'.*$', p.stdout.strip())).group(0)[:200])
    p = subprocess.run([sys.executable, os.path.join(SNIPER_ROOT, 'tools', 'cpistack.py'), '--cpi', '-d', simdir,
                        '-o', os.path.join(outdir, 'cpi-stack')], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if 'not in all_items' in p.stdout:
        return False, 'cpistack left components out: %s' % re.search(r'Also found.*', p.stdout).group(0)[:200]
    m = re.search(r'^\s*total\s+([0-9.]+)', p.stdout, re.M)
    cycles = sum(float(v) for v in st.get('performance_model.cycle_count', [])) / max(len(insns), 1)
    cpi = cycles / (sum(insns) / float(len(insns)))
    if not m or abs(float(m.group(1)) - cpi) > 0.05 * cpi:
        return False, 'cpistack total %s, the run\'s CPI %.2f' % (m.group(1) if m else 'missing', cpi)
    return True, 'sim.out and the CPI stack agree with sim.stats (CPI %s)' % m.group(1)


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
    cfg = pathlib.Path(os.path.join(root, 'sim.cfg')).read_text() if root else ''
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
    t = pathlib.Path(txt).read_text()
    m = re.search(r'^Processor:.*?Runtime Dynamic = ([0-9.]+) W', t, re.S | re.M)
    if not m or float(m.group(1)) <= 0:
        return False, 'power.txt has no processor runtime power'
    if 'name="x86" value="1"' not in pathlib.Path(xml).read_text():
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
def page_colour_check(out, rc, outdir):
    """Two copies of a chase that fits the private L2 miss no more than one:
    each application's frames cover the L2's colours evenly (D10).  With the
    old fault-order allocation two runs in four missed 17 to 37 times as often."""
    def misses(d):
        try:
            for line in io.StringIO(pathlib.Path(os.path.join(outdir, d, 'simulation', 'sim.stats')).read_text()):
                if line.startswith('L2.load-misses-data = '):
                    return [int(v) for v in line.split('=', 1)[1].split(',')]
        except OSError:
            pass
        return None
    if rc != 0:
        return False, 'make run exited %s' % rc
    one = misses('one')
    if not one:
        return False, 'no statistics for the chase alone'
    got = []
    for i in (1, 2, 3):
        two = misses('two-%d' % i)
        if not two or len(two) != 2:
            return False, 'no statistics for two copies, run %d' % i
        if max(two) > one[0] * 1.05:
            return False, 'two copies missed %s times in the L2 against %d alone (run %d)' % (two, one[0], i)
        got.append(max(two))
    return True, 'L2 misses per core: %d alone, %s as two applications' % (one[0], ', '.join(map(str, got)))
page_colour_check.needs_outdir = True


def prints(needle, note):
    def check(out, rc, needle=needle, note=note):
        return (True, note) if needle in out else (False, 'missing %r' % needle)
    check.needs_simulation = True
    return check


def post_roi_check(out, rc):
    """test/post-roi: after the ROI the clock went on (1 ms of spinning on
    clock_gettime ended), two threads started after it were joined, and a
    10 ms timed wait timed out in simulated time."""
    m = re.search(r'roi_time: elapsed_ns=(\d+) reads=(\d+) tsc_moved=(\d)', out)
    n = re.search(r'roi_threads: joined=(\d+) timedwait=(\w+) waited_ns=(-?\d+)', out)
    if rc != 0 or not m or not n:
        return False, 'exit %s; %s' % (rc, 'no roi_time line' if not m else 'no roi_threads line' if not n else 'a guest failed')
    if int(m.group(1)) < 1000000 or m.group(3) != '1':
        return False, 'after the ROI the clock moved %s ns, tsc_moved=%s' % (m.group(1), m.group(3))
    if n.group(1) != '2' or n.group(2) != 'ETIMEDOUT' or not 10000000 <= int(n.group(3)) <= 11000000:
        return False, 'after the ROI: %s threads joined, the timed wait %s after %s ns' % n.groups()
    return True, 'after the ROI: the clock went on (%s ns), 2 threads joined, the timed wait timed out at %s ns' % (m.group(1), n.group(3))
post_roi_check.needs_simulation = True


def timed_waits_check(out, rc):
    """test/timed-waits: every timed wait ended at its deadline in the guest's
    time (want <= got <= want * 1.01 + 100 us), to a file and live, the 2 s
    one included; and each guest finished"""
    if rc != 0:
        return False, 'exit %s' % rc
    lines = re.findall(r'(file: )?(?:\[app\d+\] )?(timed_raw|timed_waits): (\S+) want=(\d+) got=(\d+) ret=(-?\d+)', out)
    if out.count('timed_raw: done ok') != 2 or out.count('timed_waits: done') != 2:
        return False, 'a guest did not finish cleanly'
    if len(lines) < 30:
        return False, 'only %d waits reported' % len(lines)
    for file_, guest, name, want, got, ret in lines:
        want, got = int(want), int(got)
        if want and not want <= got <= want * 1.01 + 100000:
            return False, '%s %s %s ended after %d ns of the guest\'s time, not its %d' % ('file' if file_ else 'live', guest, name, got, want)
    return True, '%d timed waits at their deadlines in the guest\'s time, to a file and live' % len(lines)
timed_waits_check.needs_simulation = True


def roi_exit_check(out, rc):
    """Every run of the program got to its last line, printed after its ROI,
    and every recorder exited 0.  run-sniper used to kill the front end the
    moment the simulator exited, which cut the program off after its ROI
    whenever it had more to do than the simulator took to wind down (the
    join shape, nearly every time), and its recorder was reported as exiting
    with status 1."""
    runs = len(re.findall(r'roi-exit: run-sniper exit status', out))
    done = len(re.findall(r'roi-exit: (?:join|exit) total=\S+ ran to its end after the ROI', out))
    if not runs:
        return False, 'no run reported'
    if done != runs:
        return False, '%d of %d runs ran to their end after the ROI' % (done, runs)
    bad = re.findall(r'roi-exit: run-sniper exit status ([1-9]\d*)', out)
    if bad:
        return False, 'run-sniper exited with status %s' % ', '.join(sorted(set(bad)))
    m = re.search(r'The recorder for application \d+ exited with status -?\d+', out)
    if m:
        return False, m.group(0)
    return True, '%d of %d runs ran to their end after the ROI' % (done, runs)
roi_exit_check.needs_simulation = True


def exitalive_check(out, rc):
    """test/exitalive: main returns while four workers still compute, under
    the SDE front end at -n 5 and -n 2.  Every run printed main's line and
    exited 0, and no recorder reported a status.  It hung every time: aborted
    workers parked in the barrier (-n 5), and workers that never had a core
    left their recorder threads waiting for an answer (-n 2)."""
    runs = re.findall(r'exitalive: run-sniper -n (\d+) exit status (\d+)', out)
    if not runs:
        return False, 'no run reported'
    printed = out.count('exitalive: main returns')
    if printed != len(runs):
        return False, 'main\'s line printed in %d of %d runs' % (printed, len(runs))
    bad = ['-n %s: %s' % r for r in runs if r[1] != '0']
    if bad:
        return False, 'run-sniper exited non-zero (%s)' % ', '.join(bad)
    m = re.search(r'The recorder for application \d+ exited with status -?\d+', out)
    if m:
        return False, m.group(0)
    return True, '%d runs (-n %s) ran to their end' % (len(runs), '/'.join(sorted(set(r[0] for r in runs), reverse=True)))
exitalive_check.needs_simulation = True


def timers_check(out, rc):
    """test/timers: every timer fired at its expiry in the guest's time
    (want <= got <= want * 1.01 + 100 us), to a file and live"""
    if rc != 0:
        return False, 'exit %s' % rc
    lines = re.findall(r'(file: )?(?:\[app\d+\] )?(timers_raw|timers): (\S+) want=(\d+) got=(\d+) ret=(-?\d+)', out)
    if out.count('timers_raw: done ok') != 2 or out.count('timers: done') != 2:
        return False, 'a guest did not finish cleanly'
    if len(lines) != 2 * (3 + 4):
        return False, '%d timers reported, not 14' % len(lines)
    for file_, guest, name, want, got, ret in lines:
        want, got = int(want), int(got)
        if not want <= got <= want * 1.01 + 100000:
            return False, '%s %s %s fired after %d ns of the guest\'s time, not its %d' % ('file' if file_ else 'live', guest, name, got, want)
    return True, '%d timers at their expiries in the guest\'s time, to a file and live' % len(lines)
timers_check.needs_simulation = True


def dr_timed_waits_check(out, rc):
    """test/dr-timed-waits: test/timed-waits under the DynamoRIO front end
    (timed_waits_check), to a file and live"""
    if out.count('[RECORD-TRACE] Using DynamoRIO frontend') < 4:
        return False, 'not every run used the DynamoRIO front end'
    ok, why = timed_waits_check(out, rc)
    return ok, why + ', under DynamoRIO'
dr_timed_waits_check.needs_simulation = True


def dr_timers_check(out, rc):
    """test/dr-timers: test/timers under the DynamoRIO front end
    (timers_check), and the clocks: time_probe's clock_gettime and
    gettimeofday (the vDSO's) start at the time zero, osemu/time_start --
    the one-IPC model's to a file, the simulator's live -- and its TSC
    moves with them (one cycle a nanosecond to a file, the global clock's
    2.66 GHz live, as under the SIFT recorder), never the host's"""
    if out.count('[RECORD-TRACE] Using DynamoRIO frontend') < 6:
        return False, 'not every run used the DynamoRIO front end'
    ok, why = timers_check(out, rc)
    if not ok:
        return ok, why
    probes = re.findall(r'(file: )?(?:\S+ )?(?:\[app\d+\] )?time_probe: realtime=(\d+)\.\d+ mono=(\d+)\.\d+ tod=(\d+)\.\d+ dreal=(-?\d+) dtsc=(\d+)', out)
    if len(probes) != 2:
        return False, '%d time_probe lines, not 2 (to a file and live)' % len(probes)
    for file_, real, mono, tod, dreal, dtsc in probes:
        what = 'file' if file_ else 'live'
        if not int(real) == int(mono) == int(tod) == TIME_START_DEFAULT:
            return False, '%s: the clocks start at %s, %s and %s s, not at the time zero %d' % (what, real, mono, tod, TIME_START_DEFAULT)
        dreal, dtsc = int(dreal), int(dtsc)
        ghz = 1.0 if file_ else 2.66
        if not 100000 <= dreal <= 2000000 or not abs(dtsc - dreal * ghz) <= 0.05 * dreal * ghz:
            return False, '%s: %d ns and %d TSC cycles over a 100,000-iteration loop' % (what, dreal, dtsc)
    return True, why + ', under DynamoRIO; the clocks from the time zero, to a file and live'
dr_timers_check.needs_simulation = True


def dr_refuse_time_check(out, rc):
    """test/dr-refuse-time: the DynamoRIO front end with -refuse_time, as
    on an ISA where it does not keep the guest's time: a program that uses
    no time, a poll that does not wait and counter reads run (the counter
    reads the instructions run, never the host's TSC); a vDSO clock, the
    clock system call, a sleep and a poll with a timeout each end the run
    (exit 1) with the reason named"""
    exits = dict(re.findall(r'time_uses: mode (\w+) exit (\d+)', out))
    if len(exits) != 7:
        return False, '%d of the 7 runs ended' % len(exits)
    for m in ('none', 'poll0', 'rdtsc'):
        if exits[m] != '0' or ('time_uses: %s done' % m) not in out:
            return False, '%s: exit %s, it should run to its end' % (m, exits[m])
    tsc = re.search(r'time_uses: tsc=(\d+) dtsc=(\d+)', out)
    if not tsc or not 0 < int(tsc.group(2)) < 100000 or int(tsc.group(1)) > 10**10:
        return False, 'the counter read %s, not the instructions run' % (tsc.group(0) if tsc else 'nothing')
    for m, what in (('vdso', "the vDSO's clock_gettime"), ('raw', 'clock_gettime'), ('sleep', 'clock_nanosleep'),
                    ('poll', 'poll (with a timeout)')):
        if exits[m] != '1':
            return False, '%s: exit %s, not 1' % (m, exits[m])
        if ('refused the run: the guest used time (%s)' % what) not in out:
            return False, '%s: refused without the reason (%s)' % (m, what)
    return True, '3 runs without time ran (counter %s instructions over the loop), 4 that used time refused with the reason' % tsc.group(2)
dr_refuse_time_check.needs_simulation = True


def exits_zero(out, rc):
    return (rc == 0, 'exit %s' % rc)
exits_zero.needs_simulation = True


def sleepexit_check(out, rc):
    """Both runs of test/sleepexit end: the main thread sleeping while its
    worker exits, then the worker sleeping too.  A thread in an emulated
    sleep when the last running thread exited slept forever -- nothing moved
    simulated time to its wake-up -- and the case ran to its timeout."""
    n = len(re.findall(r'sleepexit: joined$', out, re.M))
    if rc != 0 or n != 2:
        return False, 'exit %s, %d of 2 runs joined their worker' % (rc, n)
    sleeps = re.findall(r'sleepexit: (\d+) sleeps, done=1$', out, re.M)
    return True, 'both runs joined (%s sleeps)' % ' and '.join(sleeps)
sleepexit_check.needs_simulation = True


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
            for line in io.StringIO(pathlib.Path(path).read_text()):
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
    'fft':             (fft_and(simout_feature), None),
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
    'page-colour':     (page_colour_check, None),
    'fork':            (prints('Hello world', 'forked child ran'), None),
    'spinloop':        (prints('done', 'threads finished'), None),
    'post-roi':        (post_roi_check, None),
    'timed-waits':     (timed_waits_check, None),
    'timers':          (timers_check, None),
    'dr-timed-waits':  (dr_timed_waits_check, 'dynamorio'),
    'dr-timers':       (dr_timers_check, 'dynamorio'),
    'dr-refuse-time':  (dr_refuse_time_check, 'dynamorio'),
    'roi-exit':        (roi_exit_check, None),
    'sleepexit':       (sleepexit_check, None),
    'exitalive':       (exitalive_check, None),
    'true':            (exits_zero, None),
    'sniper-in-sniper': (sniper_in_sniper_check, None),
}


def unavailable(reason):
    if reason == 'mpich':
        # Fetched on demand (linked from the dependency cache, or built
        # there once), as the cases need it, rather than skipped
        sys.path.insert(0, os.path.join(SNIPER_ROOT, 'tools'))
        import run_sniper
        if not run_sniper.ensure_mpich(SNIPER_ROOT, jobs=len(os.sched_getaffinity(0))):
            return 'no MPI, and deps/sniper-deps --mpi fetch failed'
    if reason == 'torch':
        # The nn branch predictor needs libtorch, which the build carries only
        # with TORCH=ON; the build's own config says whether it does.
        build = os.environ.get('SNIPER_BUILD_DIR', os.path.join(SNIPER_ROOT, 'build', 'default'))
        try:
            cfg = pathlib.Path(os.path.join(build, 'config', 'sniper.py')).read_text()
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


def tracked_files(d):
    """The files of directory d that belong to the tree: git's list when the
    tree is a checkout -- what it tracks, and what it would track (untracked
    and not ignored: a file being added, which the case may need) --
    otherwise everything in it."""
    try:
        files = []
        for extra in ([], ['--others', '--exclude-standard']):
            out = subprocess.run(['git', 'ls-files', '-z'] + extra + ['--', '.'], cwd=d, stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, check=True).stdout
            files += [f for f in out.decode().split('\0') if f]
        if files:
            return sorted(set(files))
    except (OSError, subprocess.CalledProcessError):
        pass
    return [os.path.relpath(os.path.join(r, f), d) for r, _, fs in os.walk(d) for f in fs]


def private_tree(name, root):
    """A copy of test/<name> to run make in, so that a case writes nothing into
    the source tree.  make run used to build, record and simulate in
    test/<name>/: binaries, pinballs, traces and logs were left in the tree,
    and sniper-in-sniper's fft.sift was recorded there while test_virtuoso
    read it (E10).  The copy sits in a root of its own whose other entries
    are links to the real one, so a Makefile's ../../run-sniper and
    ../shared still resolve; the test directories it reaches into (../fft,
    ../smc-rewrite, ../pa) are copied too, since it builds in them.  Only
    files of the tree are copied, not what an earlier run in place left.
    It is beside the case's output directory, not in it: the checks search
    that for the run's statistics."""
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(os.path.join(root, 'test'))
    for e in os.listdir(SNIPER_ROOT):
        if e != 'test':
            os.symlink(os.path.join(SNIPER_ROOT, e), os.path.join(root, e))
    makefile = pathlib.Path(os.path.join(TEST_DIR, name, 'Makefile')).read_text()
    dirs = {name, 'shared'} | set(re.findall(r'\.\./([A-Za-z0-9_.-]+)', makefile)) - {'..'}
    for d in sorted(dirs):
        src = os.path.join(TEST_DIR, d)
        if not os.path.isdir(src):
            continue
        for f in tracked_files(src):
            dst = os.path.join(root, 'test', d, f)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            s = os.path.join(src, f)
            if os.path.islink(s):
                os.symlink(os.readlink(s), dst)
            elif not os.path.exists(s):
                # tracked but deleted in this checkout: copy2 raised, and
                # the whole suite stopped
                print('warning: %s is tracked but missing; not copied' % os.path.relpath(s, SNIPER_ROOT),
                      file=sys.stderr)
            else:
                shutil.copy2(s, dst)
    return os.path.join(root, 'test', name)


def run(name, timeout, outdir):
    # Each test's run-sniper output goes to a directory of its own
    # (SNIPER_OUTPUT_DIR, honoured by run-sniper and dumpstats without -d),
    # not into test/<name>/ where the next run, or another suite, finds it.
    env = dict(os.environ, SNIPER_ROOT=SNIPER_ROOT, SNIPER_OUTPUT_DIR=outdir)
    env.setdefault('SNIPER_BUILD_DIR', os.path.join(SNIPER_ROOT, 'build', 'default'))
    t0 = time.time()
    cwd = private_tree(name, outdir + '.tree')
    with subprocess.Popen(['make', 'run'], cwd=cwd,
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
    last_tree = None
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
            kept.append(outdir + '.tree')
            last_tree = os.path.join(outdir + '.tree', 'test', name)
        else:
            shutil.rmtree(outdir, ignore_errors=True)
            shutil.rmtree(outdir + '.tree', ignore_errors=True)
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
        # Each case runs in a private copy of test/<name>, kept with its
        # output when it fails; point at it.
        print('\nstopped at the first failure; %s ran in %s'
              % (stopped_early, last_tree))
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
