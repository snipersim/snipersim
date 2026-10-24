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

Both runs are ROI-scoped to the phase loop, on one core per thread (the main
thread, which only brackets the ROI, included).  Building the workload's chase
cycles costs more instructions than all the phases together, and none of it
has phase structure, so measuring it would bury the thing being tested.

Two sizes.  `make regress_pacsim` runs the quick profile, sized so a regression
pass still notices if sampling stops helping or starts disagreeing; `make
validate_pacsim` runs the full one, whose numbers are meant to be quotable and
which simulates a full detailed baseline for each of the three modes.

Skips cleanly, with a reason, when the recorder has no Pac-Sim support.

Usage:
  python3 testing/test_pacsim.py                       # all three modes, full size
  python3 testing/test_pacsim.py --profile quick
  python3 testing/test_pacsim.py --mode loop --tolerance 0.05
"""

import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
import json
import threading

from common.failfast import FailFast, add_argument as add_failfast
from common.runlog import RunLog

SNIPER_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APPS = os.path.join(SNIPER_ROOT, 'testing', 'apps')

# The three region shapes Pac-Sim has to cope with.
#
# Pac-Sim types each region it records (sift/recorder/pacsim.h): MultiThread and
# SingleThread records are delimited by global barriers, IntraBarrier records
# are found inside a barrier interval.  A workload that only ever offers
# barriers exercises one of those paths and says nothing about the other, so
# each mode below removes a structure the previous one relied on.
#
# 'task' has no counterpart in the RecordType enum as it stands -- the recorder
# that would produce such records is not in the tree -- but the workload shape
# is the one task-parallel code presents, and it is the case with the least
# structure of the three, so it is worth having ready.
#
# Sizes come in two profiles.  'quick' is for the regression pass, where the
# question is only whether sampling still helps and still agrees; 'full' is the
# study, where the numbers are meant to be quotable.  A quick ROI still has to
# be a few tens of millions of instructions: below that a run is mostly its
# start-up and the pre-ROI warm-up, and the speedup of the sampled run is
# invisible.
MODES = {
    'barrier': {
        'src': 'phased.c',
        'what': 'barrier between every phase: a global cut point always exists',
        'quick': ['-DNPHASES=12', '-DCHASE_SLOTS=131072', '-DCHASE_STEPS=60000',
                  '-DCOMPUTE_ITERS=120000'],
        'full':  ['-DNPHASES=16'],
        # The back-edge budget is the fallback for the absence of
        # synchronisation (the paper's delta-max, meant to exceed the distance
        # between barriers): it has to be longer than this shape's longest
        # phase (the compute phase, 1.92M instructions), or it fires inside
        # the phase at the first thread's end of it and leaves the others'
        # tails to the next region -- 26% error at 1M against 0.3% whole.
        'quick_region_size': 3000,
    },
    'loop': {
        'src': 'looped.c',
        'what': 'synchronised only on entry and exit: cuts must come from loops',
        'quick': ['-DNITERS=36',  '-DCHASE_SLOTS=131072', '-DCHASE_STEPS=80000',
                  '-DCOMPUTE_ITERS=160000'],
        'full':  ['-DNITERS=24'],
        # A region ends at the first loop back-edge, on any thread, once the
        # threads have run the region size between them.  One iteration of
        # this workload is about 1.5M instructions per thread, 6M for four:
        # with 1M regions the cut fell at whichever thread next finished an
        # iteration, so the regions -- and the estimate -- depended on how
        # far apart the threads had drifted on the host (9 regions and -2%
        # error in most runs, 10 to 14 regions and up to -22% in others).
        # Two iterations' worth makes the cuts line up: 24 of 24 runs gave
        # the same 5 regions and +0.6%.  With 18 iterations that left too
        # little to skip (4 of the 5 regions measured, 1.00x to 1.05x
        # faster); with 36, 0.3% to 1.0% error and 1.4x to 1.7x faster.
        'quick_region_size': 12000,
    },
    'task': {
        'src': 'tasked.c',
        'what': 'work queue, no barriers and no outer loop: cuts come from tasks',
        'quick': ['-DNTASKS=36',  '-DCHASE_SLOTS=524288', '-DCHASE_UNIT=16000',
                  '-DCOMPUTE_UNIT=32000'],
        'full':  ['-DNTASKS=48'],
    },
}

sys.path.insert(0, os.path.join(SNIPER_ROOT, 'tools'))

# The sampler's region sizes, per profile.  The recorder's defaults (regions of
# 50M instructions, none shorter than 2M) are sized for real workloads; the
# quick profile's ROIs are a few regions of those at most (the loop workload's
# is about 75M instructions), simulated in detail nearly end to end --
# accurate, and proving nothing about sampling.  A mode can set its own region size
# ('quick_region_size'), where its loop structure needs one.  The knobs scale with the workload, and so
# does the fast-forward flow-control window: a thread may run that many
# instructions ahead of the simulator between measured regions, and the
# sampler cuts regions in the order the recorder sees them.  100K of run-ahead
# is 0.2% of a 50M region and 10% of a 1M one -- enough to put one thread's
# whole phase into a region of its own, counted as if it ran alone.
PROFILE_TRACE_ARGS = {
    'quick': ['--trace-args=-sniper:pacsim-region-size 1000',
              '--trace-args=-sniper:pacsim-min-region-size 100',
              '--trace-args=-sniper:flowff 1000'],
    'full':  [],
}

# The QEMU front end (frontend/qemu-plugin) runs the same sampler
# (sift/pacsim_sampler.cc, behind frontend/qemu-plugin/pacsim_qemu.cc), in
# user mode on a live channel: run-sniper --frontend=qemu --pacsim.  Its
# shape is 'single': one thread, phases alternating a pointer chase with
# arithmetic -- phased_st.c on x86-64 (built statically), phased_rv.c on
# RISC-V (freestanding, with the clang named by SNIPER_RISCV_CC; the QEMU
# runs it with the vector and matrix units, which this shape does not use).
# The multi-threaded shapes need a QEMU that can wait for Sniper outside its
# run loop; until that lands, the plugin refuses a second thread.  The
# recorder's knobs are passed the same way (record-trace turns
# "-sniper:name value" into the plugin's name=value); the plugin's
# fast-forward window is already 1000 instructions, so flowff is not one of
# its options and is left out.
# Its cuts come from the coarse-loop rule (pacsim-loop-min 0: a back-edge
# qualifies when its loop's last iteration ran at least 1% of the budget, so
# the loop over a phase, not the chase or compute loop inside it), with
# unconditional backward jumps admitted on RISC-V, where clang closes the
# phase loop with one: cut inside the inner loops, a region was a different
# blend of chase and compute each time and nothing clustered (RISC-V
# measured 14 regions of 15).  Twice the phases on RISC-V: its 400K regions
# repeat every four phases, and a cluster is fast-forwarded only after two
# measurements, so twelve phases leave little to skip.
SINGLE = {
    'what': 'one thread, phases alternating a pointer chase with arithmetic',
    'quick': ['-DNPHASES=12', '-DCHASE_SLOTS=131072', '-DCHASE_STEPS=60000', '-DCOMPUTE_ITERS=120000'],
    'quick_riscv64': ['-DNPHASES=24', '-DCHASE_SLOTS=131072', '-DCHASE_STEPS=60000', '-DCOMPUTE_ITERS=120000'],
    'full':  ['-DNPHASES=24'],
    'quick_aarch64': ['-DNPHASES=24', '-DCHASE_SLOTS=131072', '-DCHASE_STEPS=60000', '-DCOMPUTE_ITERS=120000'],
    'quick_region_size': {'x86_64': 1000, 'riscv64': 400, 'aarch64': 400},
    'quick_min_region_size': {'x86_64': 100, 'riscv64': 50, 'aarch64': 50},
    'quick_knobs': {'x86_64': ['-sniper:pacsim-loop-min 0'],
                    'riscv64': ['-sniper:pacsim-loop-min 0', '-sniper:pacsim-markers all'],
                    'aarch64': ['-sniper:pacsim-loop-min 0', '-sniper:pacsim-markers all']},
}
QEMU_CONFIG = {'x86_64': 'gainestown', 'riscv64': 'riscv', 'aarch64': 'cortex-a72'}


def spec_for(mode):
    return SINGLE if mode == 'single' else MODES[mode]

# The knob record-trace looks for; kept in step with it deliberately.
PACSIM_KNOB = b'sniper:pacsim'


def build_dir():
    env = os.environ.get('SNIPER_BUILD_DIR')
    for name in ([env] if env else []) + [os.path.join(SNIPER_ROOT, 'build', 'default'), SNIPER_ROOT]:
        if os.path.isfile(os.path.join(name, 'lib', 'sniper')):
            return name
    if os.path.isfile(os.path.join(SNIPER_ROOT, 'lib', 'sniper')):
        return SNIPER_ROOT
    return None


def recorder_paths():
    import glob
    pats = ('sift/recorder/obj-*/sde_sift_recorder.so',)
    out = []
    for p in pats:
        out += glob.glob(os.path.join(SNIPER_ROOT, p))
    return out


def qemu_prerequisites(arch):
    """The plugin front end with the sampler, the QEMU it loads into, and for RISC-V a compiler."""
    b = build_dir()
    if not b:
        return None, 'no build directory (run make first)'
    plugin = os.path.join(b, 'lib', 'libsniper-qemu.so')
    if not os.path.isfile(plugin):
        return None, 'the QEMU plugin front end is not built (make QEMU=ON)'
    with open(plugin, 'rb') as f:
        if b'pacsim-region-size' not in f.read():
            return None, 'the QEMU plugin front end does not carry the sampler'
    var, path = {'x86_64': ('SNIPER_QEMU_X86_64', 'qemu/build/qemu-x86_64'),
                 'riscv64': ('SNIPER_QEMU_RISCV', 'qemu-riscv/build/qemu-riscv64'),
                 'aarch64': ('SNIPER_QEMU_AARCH64', 'qemu-aarch64/build/qemu-aarch64')}[arch]
    qemu = os.environ.get(var) or os.path.join(SNIPER_ROOT, path)
    if not os.path.isfile(qemu):
        return None, 'no qemu-%s (make QEMU=ON%s, or set %s)' % (arch, {'riscv64': ' RISCV_IME=ON', 'aarch64': ' ARM=ON'}.get(arch, ''), var)
    if arch in ('riscv64', 'aarch64'):
        cc = os.environ.get('SNIPER_AARCH64_CC' if arch == 'aarch64' else 'SNIPER_RISCV_CC') or os.environ.get('SNIPER_RISCV_CC')
        if not cc or not os.path.isfile(cc):
            return None, 'no clang for %s (set SNIPER_RISCV_CC, or SNIPER_AARCH64_CC)' % arch
    return {'build': b, 'plugin': plugin}, None


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
                  '(no "%s" knob); see tools/pacsim/README.md'
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


# The highest one-minute load average seen during a mode's two runs
PEAK_LOAD = [0.0]

# A sampled run must simulate at most this share of the region of interest's
# instructions in detail: the work sampling saves, and what makes it faster
# on any host.  Read from the recorder's region list, so it does not depend on
# what else the machine is doing.
MAX_DETAIL_SHARE = 0.75


def build_workload(workdir, mode, threads, profile, frontend='pin', arch='x86_64'):
    spec = spec_for(mode)
    exe = os.path.join(workdir, mode)
    if mode == 'single':
        if arch == 'riscv64':
            cmd = [os.environ['SNIPER_RISCV_CC'], '--target=riscv64-linux-gnu', '-march=rv64gc', '-mabi=lp64d',
                   '-O1', '-ffreestanding', '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-e,_start',
                   '-o', exe, os.path.join(APPS, 'phased_rv.c')]
        elif arch == 'aarch64':
            # the same freestanding program, built for AArch64 (its syscalls have both paths)
            cmd = [os.environ.get('SNIPER_AARCH64_CC') or os.environ['SNIPER_RISCV_CC'], '--target=aarch64-linux-gnu',
                   '-O1', '-ffreestanding', '-nostdlib', '-static', '-fuse-ld=lld', '-Wl,-e,_start',
                   '-o', exe, os.path.join(APPS, 'phased_rv.c')]
        else:
            cmd = [os.environ.get('CC', 'gcc'), '-O1', '-static', '-o', exe, os.path.join(APPS, 'phased_st.c')]
        subprocess.run(cmd + ['-I', os.path.join(SNIPER_ROOT, 'include')] + spec.get(profile + '_' + arch, spec[profile]), check=True)
        return exe
    cc = os.environ.get('CC', 'gcc')
    # static under QEMU: the guest's pthread_barrier_wait is then in its own
    # symbol table, where the sampler finds its synchronisation points
    subprocess.run([cc, '-O1'] + (['-static'] if frontend == 'qemu' else []) +
                   ['-I', os.path.join(SNIPER_ROOT, 'include'),
                    '-DNTHREADS=%d' % threads] + spec[profile] +
                   ['-o', exe, os.path.join(APPS, spec['src']), '-lpthread'],
                   check=True)
    return exe


def checksum_of(out):
    """The workload's self-report, whichever of the three produced it."""
    # Printed inside the ROI, so it arrives through the simulator's output
    # stream, stamped and tagged.
    m = re.search(r'(?:phased|looped|tasked|phased_st|phased_rv): .*checksum=(\S+) visited=(\d+)', out)
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


def simulate(exe, outputdir, threads, extra, timeout, frontend='pin', arch='x86_64'):
    """Both runs are ROI-scoped, and both the same way.

    Without --roi the measurement is swamped by setup: building the chase
    cycles costs more instructions than every phase combined, and it has no
    phase structure for a sampler to work with.  Confining both runs to the
    phase loop is what makes the two numbers comparable and the accuracy figure
    mean what it says.
    """
    # One core per thread, the main thread included: it opens and closes the
    # ROI and otherwise waits, but with `threads` cores it shares one with a
    # worker, and the simulator's time-slicing of that pair is what the two
    # runs then measure.  It also made the recorder die at ROI end in most
    # runs (a fault inside Pin while the descheduled worker was still in its
    # barrier), and a run that dies before printing its checksum fails the
    # same-program check.
    cmd = [os.path.join(SNIPER_ROOT, 'run-sniper'), '--roi',
           '-n', str(threads + 1), '-c', 'gainestown', '-d', outputdir] + extra + ['--', exe]
    if frontend == 'qemu':
        # the plugin front end on a live channel; the single-threaded shape on
        # one core, the architecture's configuration
        cmd = [os.path.join(SNIPER_ROOT, 'run-sniper'), '--roi', '--frontend=qemu',
               '-n', str(1 if threads == 1 else threads + 1), '-c', QEMU_CONFIG[arch], '-d', outputdir] + extra + ['--', exe]
    # The host's load while it runs, for case_faster: wall time compares two
    # runs only when nothing else is competing for the cores
    loads = []
    done = threading.Event()
    def sample():
        while not done.wait(5):
            loads.append(os.getloadavg()[0])
    sampler = threading.Thread(target=sample, daemon=True)
    sampler.start()
    try:
        result = run(cmd, timeout)
    finally:
        done.set()
        sampler.join()
    PEAK_LOAD[0] = max([PEAK_LOAD[0]] + loads)
    return result


def measure(p, workdir, mode, args):
    """Run one mode's workload twice -- full, then sampled -- and return both."""
    threads = 1 if mode == 'single' else args.threads
    exe = build_workload(workdir, mode, threads, args.profile, args.frontend, args.arch)

    PEAK_LOAD[0] = 0.0
    full_dir = os.path.join(workdir, mode + '-full')
    rc, out, wall_full = simulate(exe, full_dir, threads, [], args.timeout, args.frontend, args.arch)
    if rc is None:
        raise RuntimeError('the full detailed run timed out after %ds' % args.timeout)
    if rc != 0:
        raise RuntimeError('the full detailed run failed (exit %s)' % rc)
    truth_sum = checksum_of(out)
    truth = elapsed_seconds(os.path.join(full_dir, 'simulation'))

    samp_dir = os.path.join(workdir, mode + '-sampled')
    trace_args = list(PROFILE_TRACE_ARGS[args.profile])
    if args.frontend == 'qemu':
        trace_args = [a for a in trace_args if 'flowff' not in a]     # the plugin's window is 1000 already
    for knob in ('region_size', 'min_region_size'):
        value = spec_for(mode).get(args.profile + '_' + knob)
        if isinstance(value, dict):
            value = value[args.arch]
        if value:
            # Pin knobs are write-once: replace the profile's value, do not repeat it
            trace_args = [a for a in trace_args if 'pacsim-' + knob.replace('_', '-') not in a]
            trace_args.append('--trace-args=-sniper:pacsim-%s %d' % (knob.replace('_', '-'), value))
    for knob in spec_for(mode).get(args.profile + '_knobs', {}).get(args.arch, []):
        trace_args.append('--trace-args=' + knob)
    rc, out, wall_samp = simulate(exe, samp_dir, threads,
                                  ['--pacsim'] + trace_args
                                  + ['--trace-args=' + a for a in args.trace_arg], args.timeout, args.frontend, args.arch)
    if rc is None:
        raise RuntimeError('the sampled run timed out after %ds' % args.timeout)
    if rc != 0:
        raise RuntimeError('the sampled run failed (exit %s)' % rc)
    samp_sum = checksum_of(out)

    m = re.search(r'\[PacSim\] Estimated sim time ([0-9.]+)s', out)
    if not m:
        raise RuntimeError('the sampled run reported no estimate')
    estimate = float(m.group(1))

    # The after-the-fact path: tools/dumpstats.py on the sampled directory
    # must report the same estimate the run printed.
    p = subprocess.run([sys.executable, os.path.join(SNIPER_ROOT, 'tools', 'dumpstats.py'), '-d', samp_dir],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    m = re.search(r'^pacsim\.estimated_sim_time = ([0-9.e+-]+)', p.stdout, re.M)
    dumpstats_estimate = float(m.group(1)) if m else None
    dumpstats_output = p.stdout

    # The work saved: instructions of measured regions over all of them
    regions = json.load(open(os.path.join(samp_dir, 'pacsim_regions.json')))['feature_stack']
    total = sum(sum(rg['insnums']) for rg in regions)
    detailed = sum(sum(rg['insnums']) for rg in regions if rg['feature_level'] == 1)

    return {
        'truth': truth, 'estimate': estimate,
        'detail_share': (detailed / total) if total else 1.0, 'ins_total': total,
        'regions': len(regions), 'measured': sum(1 for rg in regions if rg['feature_level'] == 1),
        'peak_load': PEAK_LOAD[0],
        'wall_full': wall_full, 'wall_sampled': wall_samp,
        'checksum_full': truth_sum, 'checksum_sampled': samp_sum,
        'dumpstats_estimate': dumpstats_estimate, 'dumpstats_output': dumpstats_output,
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


def case_less_detail(r, args):
    """The sampled run simulated at most MAX_DETAIL_SHARE of the instructions in detail.

    What sampling saves, and why it is faster: the regions it extrapolates are
    only warmed, not timed.  Counted from the recorder's region list, so the
    check means the same on a busy host as on an idle one."""
    detail = '%.1f%% of %.1fM instructions in detail, %d of %d regions measured' % (
        100.0 * r['detail_share'], r['ins_total'] / 1e6, r['measured'], r['regions'])
    if r['detail_share'] > MAX_DETAIL_SHARE:
        return False, '%s, wanted at most %.0f%%' % (detail, 100.0 * MAX_DETAIL_SHARE)
    return True, detail


def case_faster(r, args):
    """The sampled run is faster in wall-clock time than the full run.

    Only on a host with cores to spare: another job competing for them stretches
    either run by however much it happens to compete, and a comparison of the
    two then measures the neighbour.  Skipped, with the load, when the host is
    busy; case_less_detail checks the work saved either way."""
    ncpu = os.cpu_count() or 1
    own = args.threads + 2          # the simulated threads, the simulator's own, the recorder
    other = r['peak_load'] - own
    if other > ncpu / 4:
        return None, 'host busy (load %.0f on %d cores, about %.0f of it other work): wall times not comparable' % (
            r['peak_load'], ncpu, other)
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


def case_dumpstats(r, args):
    """tools/dumpstats.py on the sampled directory reports the estimate the run
    printed: the one function behind both paths, reached through the tool."""
    if r['dumpstats_estimate'] is None:
        tail = r['dumpstats_output'].strip().splitlines()[-1:] or ['(no output)']
        return False, 'dumpstats printed no pacsim.estimated_sim_time: %s' % tail[0]
    if abs(r['dumpstats_estimate'] - r['estimate']) > 1e-6 * max(1.0, abs(r['estimate'])):
        return False, 'dumpstats %g s, run-sniper %g s' % (r['dumpstats_estimate'], r['estimate'])
    return True, 'dumpstats reports the same %g s' % r['estimate']


CASES = {
    'same-program': case_same_program,
    'dumpstats': case_dumpstats,
    'faster':       case_faster,
    'less-detail':  case_less_detail,
    'accurate':     case_accurate,
}

# Checks known to fail on the quick profile, each with the reason.  They run
# and are reported, and a pass is reported as such (the entry is then stale),
# but they do not fail the suite: a suite that fails for a known reason on
# every run tells nobody anything the day something else breaks.
# The task workload's three, one cause: a region's time is its busiest
# thread's, and which tasks that thread ran -- memory-bound or compute-bound
# -- decides it, while the sampler clusters regions by the mix summed over
# all threads, which looks the same either way.  Regions of one cluster
# differ by up to 1.6x in time per instruction (measured, 36 and 144 tasks),
# so a cluster does not predict a region's time, the predictor rarely trusts
# one enough to skip it, and which thread runs which task depends on timing,
# so the sampled run's schedule is not the full run's (two full runs differ
# by 1%).  Larger regions and four times the tasks did not change that (7% to
# 17% error, 68% to 90% in detail).  Fixing it is a change to the method --
# clustering on what the critical thread ran -- not to this test.  Since the
# convergence rule (pacsim-converge: a cluster is skipped only once two
# measurements of it agree) the estimate is right (0.2%), because the sampler
# measures every region of such a cluster; the run is then no shorter.
_TASK_REASON = ('regions of one cluster differ by up to 1.6x in time per instruction: a '
                'region\'s time is its busiest thread\'s, whose tasks (memory- or compute-bound) '
                'the summed mix the sampler clusters on does not show; see the note above')
KNOWN_QUICK_FAILURES = {
    ('single', 'faster'):    'under QEMU user mode the run is mostly its start-up and the pre-ROI '
                             'warm-up at this size; the ROI the sampler shortens is a small part of it',
    ('task', 'less-detail'): _TASK_REASON,
    ('task', 'faster'):      _TASK_REASON,
}


def main():
    # Line-buffer stdout.
    #
    # Python block-buffers when stdout is a file, so a redirected run writes
    # nothing for 8KB at a time.  A sweep that takes an hour then looks
    # identical to one that has hung -- same file size, same mtime, for twenty
    # minutes at a stretch -- and the only way to tell them apart is to go
    # looking at process tables.  Progress output is worth nothing if it
    # arrives after the run.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:      # pragma: no cover - Python < 3.7
        pass

    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', action='append', choices=sorted(MODES) + ['single'],
                    help='region shape to test (repeatable; default: all three on Pin, single on QEMU)')
    ap.add_argument('--frontend', choices=('pin', 'qemu'), default='pin',
                    help='the front end that records: the SDE recorder (default) or the QEMU plugin in user mode')
    ap.add_argument('--arch', choices=('x86_64', 'riscv64', 'aarch64'), default='x86_64',
                    help='with --frontend qemu: the guest architecture (default: %(default)s)')
    ap.add_argument('--case', action='append', choices=sorted(CASES))
    ap.add_argument('--profile', choices=('quick', 'full'), default='full',
                    help='workload size (default: %(default)s); quick is what '
                         'the regression pass uses')
    ap.add_argument('--threads', type=int, default=4)
    ap.add_argument('--tolerance', type=float, default=0.10,
                    help='allowed relative error in the estimate (default: %(default)s)')
    ap.add_argument('--min-speedup', type=float, default=1.0,
                    help='minimum acceptable wall-clock speedup (default: %(default)s)')
    ap.add_argument('--timeout', type=int, default=3600)
    ap.add_argument('--trace-arg', action='append', default=[], metavar='KNOB',
                    help='an extra recorder knob for the sampled run, e.g. "-sniper:pacsim-warmup 0"')
    ap.add_argument('--keep', action='store_true', help='keep the working directory')
    add_failfast(ap)
    args = ap.parse_args()

    log = RunLog('pacsim')
    stopped_early = False

    p, reason = prerequisites() if args.frontend == 'pin' else qemu_prerequisites(args.arch)
    if not p:
        print('SKIP: %s' % reason)
        log.finish()
        return 0

    modes = args.mode or (sorted(MODES) if args.frontend == 'pin' else ['single'])
    if args.frontend == 'pin' and 'single' in modes:
        print('SKIP: the single-threaded shape is the QEMU front end\'s (--frontend qemu)')
        log.finish()
        return 0
    names = args.case or sorted(CASES)
    workdir = tempfile.mkdtemp(prefix='sniper-pacsim-')
    failures = 0
    checks = 0
    try:
        for mode in modes:
            # Announced before the work, flushed, so the log always names what
            # is in flight: measure() simulates the workload twice and is where
            # all the time goes.
            print('=== %s: %s ===' % (mode, spec_for(mode)['what']))
            print('  simulating (full detailed baseline, then sampled) ... ', end='')
            sys.stdout.flush()
            started = time.time()
            try:
                r = measure(p, workdir, mode, args)
            except RuntimeError as e:
                log.record('%s (simulate)' % mode, time.time() - started, 'FAIL')
                print('ERROR (%s)' % e)
                failures += 1
                if args.failfast:
                    raise FailFast()
                continue
            # The simulation is the unit worth timing: it runs the workload
            # twice, and the checks below only read what it left behind.
            took = log.record('%s (simulate)' % mode, time.time() - started)
            print('done [%ds]' % round(took))
            for name in names:
                ok, detail = CASES[name](r, args)
                if ok is None:
                    print('  %-12s SKIP (%s)' % (name, detail))
                    continue
                known = KNOWN_QUICK_FAILURES.get((mode, name)) if args.profile == 'quick' else None
                if known and not ok:
                    verdict = 'XFAIL'
                    detail += '; known: ' + known
                elif known:
                    verdict = 'XPASS'
                    detail += '; listed as a known failure, no longer failing'
                else:
                    verdict = 'PASS' if ok else 'FAIL'
                print('  %-12s %s (%s)' % (name, verdict, detail))
                checks += 1
                failures += 0 if (ok or known) else 1
                if not ok and not known and args.failfast:
                    raise FailFast()
            print()

        if failures:
            print('%d of %d checks FAILED' % (failures, checks))
            return 1
        print('all %d checks passed' % checks)
        return 0
    except FailFast:
        stopped_early = True
        print('%d of %d checks FAILED' % (failures, checks))
        return 1
    finally:
        log.finish()
        if args.keep or stopped_early:
            if stopped_early:
                print('stopped at the first failure;', end=' ')
            print('working directory kept: %s' % workdir)
        else:
            import shutil
            shutil.rmtree(workdir, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
