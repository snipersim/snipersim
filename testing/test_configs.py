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

Fragments that select a particular model variant (an SMT core, a prefetcher,
a replacement policy, a DRAM model, a directory type, a scheduler) also have
the detailed run's statistics checked: the counter that only that variant
registers or drives must be non-zero (FRAGMENT_REQUIRE_NONZERO), or the line
only it prints must appear (FRAGMENT_EXPECT_OUTPUT).  A run that completed
with the default model quietly in place looks exactly like one that used the
variant, and this is what tells them apart.

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
import signal
import subprocess
import sys
import tempfile
import time

from common.failfast import FailFast, add_argument as add_failfast
from common.runlog import RunLog

HERE = os.path.dirname(os.path.abspath(__file__))
SNIPER_ROOT = os.path.dirname(HERE)
CONFIG_DIR = os.path.join(SNIPER_ROOT, 'config')
WORKLOAD_SRC = os.path.join(HERE, 'apps', 'workload.c')
# The scripting suite's workload (testing/apps/threaded.c): a main thread and
# four workers.  For the fragments that have nothing to do with one thread --
# the schedulers, and SMT, where the second thread is the point.
THREADED_SRC = os.path.join(HERE, 'apps', 'threaded.c')

# Printed by each workload once it has finished; its absence means the run
# produced nothing even if the exit status was zero.
WORKLOAD_OUTPUT = {
    'workload': re.compile(r'workload isum=1419064784 fsum=100638720\.000'),
    'threaded': re.compile(r'threads=4 total=37\.589880'),
}

# 1, 2 and 4.  Four used to be left out because gainestown -- an L3 shared by
# four cores -- aborted at -n 4 on the tag-directory placement check.  That was
# an uninitialised flag in the memory manager, fixed, and a quad-core config
# that is never run at four cores is exactly the gap this suite exists to close.
DEFAULT_CORES = (1, 2, 4)

# Complete machines: must run on their own.
EXPECT_PASS = [
    'dunnington', 'dunnington16', 'gainestown', 'nehalem-lite', 'silvermont',
    # the RISC-V core model is built by default (RISCV=ON); the decoder follows
    # the ISA the trace declares, so an x86 workload runs on these too
    'riscv', 'riscv-mediumboom',
]

# Overlay fragments: tune one aspect and must be layered on a complete config.
# default.cfg is an empty file and smt1.cfg is two lines, so running them alone
# was never meaningful.
EXPECT_PASS_LAYERED = [
    'atd', 'cacheonly', 'dram-cache', 'interval', 'jikes', 'llc-qbs',
    'nehalem', 'noc', 'nuca-cache', 'oneipc', 'prefetcher', 'rob',
    'smt1', 'smt2', 'smt4', 'smt8',
    # the two-node NUMA topology for the tiered DRAM controller
    'numa',
]
LAYER_BASE = 'gainestown'
LAYER_BASE_BY_MODE = {}

# Overlays needing something between the base and themselves.
LAYERED_EXTRA_BASE = {
    # Its nodes are DDR models, and no complete machine config sets the
    # [perf_model/dram/ddr] keys (gainestown's DRAM is the constant model):
    # the DRAM fragment that does goes in front, as numa.cfg documents.
    'numa': ['dram_configs/ddr4_2400'],
}

# Per-overlay layering base, for overlays that cannot sit on the default one.
# Empty: oneipc used to be sent to nehalem-lite because it tripped the
# tag-directory placement check on gainestown; it no longer does, at either
# core count, in either mode.
LAYER_BASE_OVERRIDE = {}

# Cannot work in a plain x86 SDE build, with the reason.  Listed rather than
# skipped, so that one starting to work is reported too.
EXPECT_FAIL = {
    'beckton': 'cache sizes are not a power of two '
               '("Caches of non-power of 2 size need funky hash function")',
    'cortex-a53': 'ARM target: needs an ARM frontend and an ARM trace',
    'cortex-a72': 'ARM target: needs an ARM frontend and an ARM trace',
    'kingscross': 'mesh topology fixed at 16:4, so it needs 64 cores',
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
EXPECT_FAIL_DETAILED = {
    # The RISC-V configurations select the RISC-V decoder (general/arch), and
    # this suite's workload is an x86 binary: fast-forward never decodes, the
    # timing model does and asserts on the first x86 instruction.  They are for
    # RISC-V traces (docs/RISCV_AI_ACCELERATOR_STACK.md); the riscv-* cases of
    # test_qemu.py cover them.
    'riscv': 'the RISC-V decoder cannot decode an x86 trace',
    'riscv-mediumboom': 'the RISC-V decoder cannot decode an x86 trace',
}

# Not a configuration to run on its own.
EXCLUDE = {'base'}

# Fragments in config/ subdirectories.
#
# shipped_configs() lists config/*.cfg and so never saw these: 24 files across
# eight directories, and the newer work is almost all of it -- CXL, DRAM models,
# page tables, physical memory allocators, address translation, the MMU, a core
# model, and both MimicOS system configs.  They layer the same way the overlays
# above do; they were simply never enumerated.
#
# Probed rather than assumed: every one was run layered and standalone, and the
# results are recorded here.  The fragments and schemes added for the
# test_virtuoso.py variant checks -- the stride prefetcher, the Revelator
# open-addressing and THP engines, the revelator_simple, revelator_thp and
# numa_reserve_thp allocators, page-size prediction and the no-PWC filter --
# were probed the same way, layered on gainestown in both modes at 1, 2 and
# 4 cores, and all pass; that is why none of them appears below.  The engine
# fragments under spec_engine_configs/ are not enumerated, as before: an
# engine only runs under the spec MMU with its allocator, and the schemes
# that layer all three are what gets run.
FRAGMENT_DIRS = ('address_translation_schemes', 'common_configs', 'core_configs',
                 'cxl_configs', 'dram_configs', 'mmu_configs', 'pagetable_configs',
                 'physical_memory_allocators',
                 # The variants that were compiled but selected by nothing: the
                 # L2 replacement policies, the GHB prefetcher and the
                 # sequential cache timing model; the limited and LimitLESS
                 # directories; the roaming and big/small schedulers.  (The
                 # SMT ROB core, the nn branch predictor and the normal DRAM
                 # model and DDR address mappings sit in the directories above.)
                 'cache_configs', 'directory_configs', 'scheduler_configs')

# Fragments needing something in front of them beyond the usual base.
FRAGMENT_EXTRA_BASE = {
    # Deltas on cxl.cfg: they override a few keys and need it for the rest.
    'cxl_configs/cxl-asic': ['cxl_configs/cxl'],
    'cxl_configs/cxl-fpga': ['cxl_configs/cxl'],
    # Deltas on ddr4_2400.cfg: the other three address mappings of the
    # detailed DDR model (ddr4_2400 selects xor).
    'dram_configs/ddr4_2400_open_page': ['dram_configs/ddr4_2400'],
    'dram_configs/ddr4_2400_open_page_split_column': ['dram_configs/ddr4_2400'],
    'dram_configs/ddr4_2400_closed_page': ['dram_configs/ddr4_2400'],
}

# Fragments that need something the build may not have, with its name.  A
# missing feature SKIPS the case with the reason rather than failing it: the
# nn branch predictor is compiled only when the build has libtorch
# (SNIPER_ENABLE_TORCH defines HAS_LIBTORCH), and a build without it is a
# legitimate build, not a broken one.  build_features() reads what the build
# that lib/sniper points at was configured with.
FRAGMENT_REQUIRES = {
    'core_configs/nn_branch_predictor': 'torch',
}

# Detailed-mode timeouts for fragments whose timing model is slower than the
# two minutes --timeout-detailed allows a healthy run.  The nn predictor
# trains a libtorch model on every batch of branches: about 100 s for this
# workload on an idle host, against ten for the others.
FRAGMENT_TIMEOUT_DETAILED = {
    'core_configs/nn_branch_predictor': 600,
}

# Fragments run on the threaded workload.  A scheduler with one thread never
# schedules, and an SMT core with one thread never shares.
FRAGMENT_WORKLOAD = {
    'core_configs/rob_smt2': 'threaded',
    'scheduler_configs/roaming': 'threaded',
    'scheduler_configs/big_small': 'threaded',
    'scheduler_configs/sequential': 'threaded',
}

# Core counts for fragments that cannot use the default ones; replaces --cores
# for that fragment.
FRAGMENT_CORES = {
    # logical_cpus = 2 pairs cores 2k and 2k+1 into one physical SMT core, so
    # one logical core has no partner and rob_timer.cpiSMT stays zero there.
    'core_configs/rob_smt2': (2, 4),
    # Tags cores 0 and 1 big and 2 and 3 small; with fewer than three cores
    # there is no small core for the threads to start on.
    'scheduler_configs/big_small': (4,),
    # Its sequence "1|1|1|2" has four slots, one core each: the scheduler
    # refuses to start with fewer cores than slots.
    'scheduler_configs/sequential': (4,),
}

# Known to fail in detailed mode at one core count only, with the reason;
# listed rather than skipped so that one starting to work is reported.
FRAGMENT_EXPECT_FAIL_DETAILED_CORES = {
    # Two logical cores are one physical SMT core, and five threads on it are
    # time-sliced by the scheduler; the run dies with SIGSEGV in
    # RobSmtTimer::issueInstruction -> Core::accessMemory ->
    # Core::initiateMemoryAccess (core.cc:375) about a second into the ROI,
    # after the application reports "The futex facility returned an
    # unexpected error code".  Deterministic at two logical cores; at four
    # (two physical) it passed in isolation and crashed the same way in a
    # full sweep on a loaded host, so the crash is the SMT timer's, not the
    # core count's.  Absent with the interval core's SMT model (smt2 on
    # gainestown) and with a single thread.  The defect stands until the
    # SMT ROB timer is fixed; these entries make the suite say when it is.
    'core_configs/rob_smt2': {2: 'SIGSEGV in RobSmtTimer::issueInstruction with '
                                 'five threads time-sliced on one physical SMT core'},
}

# Known to fail intermittently in detailed mode at one core count: the run is
# reported as passed either way, with the outcome of this run in the note, so
# the suite stays quiet while the defect is on record and loud about nothing.
FRAGMENT_INTERMITTENT_DETAILED_CORES = {
    # The same crash as above at four logical cores (two physical): it passed
    # in isolation and crashed the same way in a full sweep on a loaded host.
    'core_configs/rob_smt2': {4: 'SIGSEGV in RobSmtTimer::issueInstruction, '
                                 'deterministic at one physical core'},
}

# Counters that must be non-zero after the detailed run: the one each variant
# registers (or drives) that nothing else does.  Checked in detailed mode only
# -- fast-forward never attaches the timing models, so every one of these
# legitimately reads zero there.  The config suite used to check only that
# the run completed; a variant whose factory case was silently falling
# through to the default would have passed.
#
# How each one discriminates:
#   rob_smt2    cpiSMT is registered by the SMT ROB timer alone (rob_timer.cc
#               has every other name); it accumulates when a thread could have
#               dispatched but its SMT sibling already had, so it needs the
#               threaded workload on a core with a partner.
#   nn          the nn predictor builds a Pentium M predictor as its alternate
#               under the name branch_predictor_alternate; no other predictor
#               creates that object.
#   ghb         the GHB prefetcher registers nothing of its own; the cache
#               controller counts what it issues in L2.prefetches, which is
#               zero without a prefetcher.
#   mplru/chirp register their own counters under the cache's name.
#   normal      DramPerfModelNormal's own registration (same names as the
#               constant model's, which it replaces).
#   mappings    the detailed DDR model's row-buffer accounting, which the
#               mapping decides.
#   directories entries-allocated is the directory's own count; the limited
#               types only differ at the sharer limit, which a single-threaded
#               workload never reaches (nothing is shared), so this says the
#               directory was built and used, not that the limit was hit.
#   sequential  the per-thread statistic SchedulerSequential alone registers
#               (ThreadStatNamedStat "l1d_load_miss", the L1-D's load-misses
#               total while the thread was on the core), which is what its
#               constructor used to assert on when the cache controller did
#               not register that total.  Non-zero means the lookup resolved
#               and the per-thread accounting ran.
FRAGMENT_REQUIRE_NONZERO = {
    'core_configs/rob_smt2': ('rob_timer.cpiSMT',),
    'core_configs/nn_branch_predictor': ('branch_predictor_alternate.num-correct',
                                         'branch_predictor_alternate.num-incorrect'),
    'cache_configs/prefetcher_ghb': ('L2.prefetches', 'L2.hits-prefetch'),
    'cache_configs/replacement_mplru': ('L2.mplru-data-evicted',
                                        'L2.mplru-metadata-protected'),
    'cache_configs/replacement_chirp': ('L2.chirp-dead-correct', 'L2.chirp-bypass-lru'),
    'dram_configs/normal': ('dram.total-access-latency', 'dram.total-queueing-delay'),
    'dram_configs/ddr4_2400_open_page': ('ddr.page-hits', 'ddr.page-miss'),
    'dram_configs/ddr4_2400_open_page_split_column': ('ddr.page-hits', 'ddr.page-miss'),
    'dram_configs/ddr4_2400_closed_page': ('ddr.page-hits', 'ddr.page-miss'),
    'directory_configs/limited_no_broadcast': ('directory.entries-allocated',),
    'directory_configs/limitless': ('directory.entries-allocated',),
    'scheduler_configs/sequential': ('thread.l1d_load_miss', 'thread.l1d_store_miss'),
}

# Lines the run must print, for variants whose evidence is their output.
FRAGMENT_EXPECT_OUTPUT = {
    # big_small/debug = true: the scheduler reports each promotion decision.
    'scheduler_configs/big_small': re.compile(r'\[SchedulerBigSmall\] thread \d+ (stalled|ended)'),
    # sequential/verbose = true: thread 4 (the fourth worker) is queued behind
    # thread 3 on core 3 and scheduled when it exits, and the end-of-run
    # [REPORT] has a line per thread.  Both in one pattern, since a fragment
    # has one; the order is that of the run (the report comes last).
    'scheduler_configs/sequential': re.compile(
        r'\[SCHED\] 4: Pushed to core queue 3\b.*\[SCHED\] 4: Scheduled\..*'
        r'\[REPORT\]\[THREAD:4\] +CYCLES: \d+ TIME:\d+ INSTR:\d+ L1D-miss:\d+', re.S),
    # hugetlbfs: the service is built with its backing criterion.  Only a
    # trace-driven run has VMAs for the pool to back (test_virtuoso.py's
    # hugetlbfs case), so with this suite's live workload the service stays
    # inert and its evidence is that it initialised with the criterion.
    'address_translation_schemes/hugetlbfs':
        re.compile(r'\[HUGETLBFS\]\s+Backing VMAs of at least 4 MB'),
}

# Fragments that cannot work, with the reason.  Listed rather than skipped, so
# that one starting to work is reported too.
FRAGMENT_EXPECT_FAIL = {
    'dram_configs/m3d':
        'needs perf_model/dram/ddr/address_mapping, which no shipped config sets',
    'dram_configs/readwrite_2ctrl_153gbps':
        'two memory controllers, but they may only sit at the master node of a '
        'shared cache, which no shipped base provides',
    'mmu_configs/mmu_spec':
        'the speculative MMU needs an engine and the allocator it predicts '
        'from (address_translation_schemes/revelator layers all three)',
    'mmu_configs/mmu_virt':
        'the virt MMU nests on a host MMU and a guest OS, which only '
        'general/virtualized_environment creates, with a guest page table '
        'and allocator (address_translation_schemes/virt layers all of it)',
}


def shipped_fragments():
    out = []
    for d in FRAGMENT_DIRS:
        path = os.path.join(CONFIG_DIR, d)
        if not os.path.isdir(path):
            continue
        for f in sorted(os.listdir(path)):
            if f.endswith('.cfg'):
                out.append('%s/%s' % (d, os.path.splitext(f)[0]))
    return out


def shipped_configs():
    return sorted(os.path.splitext(f)[0]
                  for f in os.listdir(CONFIG_DIR) if f.endswith('.cfg'))


def build_workload(workdir):
    cc = os.environ.get('CC', 'gcc')
    exe = os.path.join(workdir, 'workload')
    subprocess.run([cc, '-O1', '-o', exe, WORKLOAD_SRC], check=True)
    threaded = os.path.join(workdir, 'threaded')
    subprocess.run([cc, '-O1', '-pthread', '-o', threaded, THREADED_SRC], check=True)
    return {'workload': exe, 'threaded': threaded}


def build_features():
    """What the build behind lib/sniper was configured with, as a set.

    lib/sniper is a symlink into the build directory (build/default by
    default, BUILD_DIR otherwise); its CMakeCache.txt records every
    SNIPER_ENABLE_* option.  Read from there rather than guessed from the
    source tree, because the question is what this binary can do.
    """
    exe = os.path.realpath(os.path.join(SNIPER_ROOT, 'lib', 'sniper'))
    cache = os.path.join(os.path.dirname(os.path.dirname(exe)), 'CMakeCache.txt')
    feats = set()
    if os.path.exists(cache):
        for line in open(cache):
            m = re.match(r'SNIPER_ENABLE_(\w+):BOOL=(ON|TRUE|1)\s*$', line.strip())
            if m:
                feats.add(m.group(1).lower())
    return feats


def read_stats(outdir):
    sys.path.insert(0, os.path.join(SNIPER_ROOT, 'tools'))
    import sniper_lib
    return sniper_lib.get_results(resultsdir=outdir)['results']


def counter(stats, name):
    """A statistic as a plain int, summed over cores.  0 when absent."""
    v = stats.get(name)
    if v is None:
        return 0
    if isinstance(v, (list, tuple)):
        return sum(int(x) for x in v)
    return int(v)


def check_nonzero(outdir, names):
    """Every named counter must exist and be non-zero (test_virtuoso's
    require_nonzero, on a results directory)."""
    try:
        stats = read_stats(outdir)
    except Exception as e:
        return False, 'cannot read statistics: %s' % e
    missing = [n for n in names if n not in stats]
    if missing:
        return False, 'counter not registered: %s' % ', '.join(missing)
    zero = [n for n in names if counter(stats, n) == 0]
    if zero:
        return False, 'counter stayed zero: %s' % ', '.join(zero)
    return True, ', '.join('%s=%d' % (n, counter(stats, n)) for n in names)



def run_in_own_group(cmd, timeout, **kwargs):
    """subprocess.run(), but kill the whole process group on timeout.

    subprocess.run(timeout=) kills only the direct child.  That child is
    run-sniper, a Python script, and the simulator it launches is a grandchild:
    on a timeout the simulator survived, detached, and kept running.  A suite
    run against a configuration that deadlocks therefore leaked one stuck
    simulator per timed-out case -- six of them, still resident after eleven
    hours, were what prompted this.

    start_new_session puts the child in its own process group so the whole
    tree can be signalled.
    """
    with subprocess.Popen(cmd, start_new_session=True, **kwargs) as p:
        try:
            out, _ = p.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                p.kill()
            p.communicate()
            raise
        return subprocess.CompletedProcess(cmd, p.returncode, out, None)

def run_config(names, ncores, workdir, workload, timeout, mode,
               expect_output=None, counters=(), app='workload'):
    tag = '+'.join(names)
    outdir = os.path.join(workdir, 'out-%s-%s-n%d' % (tag, mode, ncores))
    shutil.rmtree(outdir, ignore_errors=True)
    cmd = [os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', str(ncores)]
    if mode == 'fast-forward':
        cmd.append('--fast-forward')
    cmd += ['-c' + n for n in names]
    cmd += ['-d', outdir, '--', workload[app]]
    try:
        p = run_in_own_group(cmd, timeout, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, cwd=SNIPER_ROOT)
    except subprocess.TimeoutExpired:
        return False, 'timed out after %ds' % timeout

    # run-sniper exits 0 even when the simulator refuses to start, so the exit
    # status alone is not enough: require the workload's own output too.
    ok = p.returncode == 0 and bool(WORKLOAD_OUTPUT[app].search(p.stdout))
    if ok:
        detail = 'ok'
        if expect_output and not expect_output.search(p.stdout):
            return False, 'expected output %r not seen' % expect_output.pattern
        # The variant's counters, on the run that attached the timing models.
        if counters and mode == 'detailed':
            ok, detail = check_nonzero(outdir, counters)
        shutil.rmtree(outdir, ignore_errors=True)
        return ok, detail

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
    ap.add_argument('--only', action='append', help='check only this config (repeatable)')
    ap.add_argument('--cores', default=','.join(str(c) for c in DEFAULT_CORES),
                    help='comma-separated core counts (default: %(default)s)')
    ap.add_argument('--timeout', type=int, default=300)
    # A healthy detailed run of this workload is about ten seconds, so a
    # configuration that has not finished in two minutes has deadlocked.  The
    # shorter limit matters because the configs that do deadlock spend the whole
    # timeout proving it.
    ap.add_argument('--timeout-detailed', type=int, default=120)
    add_failfast(ap)
    ap.add_argument('--mode', action='append', choices=MODES,
                    help='run only this mode (repeatable; default: both)')
    args = ap.parse_args()

    if not os.path.exists(os.path.join(SNIPER_ROOT, 'lib', 'sniper')):
        print('error: lib/sniper not built; run make first', file=sys.stderr)
        return 2

    cores = [int(c) for c in args.cores.split(',')]
    classified = set(EXPECT_PASS) | set(EXPECT_PASS_LAYERED) | set(EXPECT_FAIL) | EXCLUDE
    unclassified = [c for c in shipped_configs() if c not in classified]
    features = build_features()

    def case(label, names, expect_ok, note, fragment=None):
        return {
            'label': label, 'names': names, 'expect_ok': expect_ok, 'note': note,
            'app': FRAGMENT_WORKLOAD.get(fragment, 'workload'),
            'cores': FRAGMENT_CORES.get(fragment, cores),
            'counters': FRAGMENT_REQUIRE_NONZERO.get(fragment, ()),
            'output': FRAGMENT_EXPECT_OUTPUT.get(fragment),
            'fail_cores': FRAGMENT_EXPECT_FAIL_DETAILED_CORES.get(fragment, {}),
            'flaky_cores': FRAGMENT_INTERMITTENT_DETAILED_CORES.get(fragment, {}),
            'requires': FRAGMENT_REQUIRES.get(fragment),
            'timeout_detailed': FRAGMENT_TIMEOUT_DETAILED.get(fragment),
        }

    def build_cases(mode):
        layer_base = LAYER_BASE_BY_MODE.get(mode, LAYER_BASE)
        out = []
        for n in EXPECT_PASS:
            out.append(case(n, [n], True, None))
        for n in EXPECT_PASS_LAYERED:
            base = LAYER_BASE_OVERRIDE.get(n, layer_base)
            if base == n:          # nehalem-lite layered onto itself is not a case
                continue
            names = [base] + LAYERED_EXTRA_BASE.get(n, []) + [n]
            out.append(case(' + '.join(names), names, True, None))
        for n in sorted(EXPECT_FAIL):
            out.append(case(n, [n], False, EXPECT_FAIL[n]))
        for n in shipped_fragments():
            names = [layer_base] + FRAGMENT_EXTRA_BASE.get(n, []) + [n]
            out.append(case(' + '.join(names), names,
                            n not in FRAGMENT_EXPECT_FAIL,
                            FRAGMENT_EXPECT_FAIL.get(n), fragment=n))
        if args.only:
            out = [c for c in out if any(o in c['names'] for o in args.only)]
        return out

    modes = args.mode or list(MODES)

    workdir = tempfile.mkdtemp(prefix='sniper-configs-')
    failures = []
    log = RunLog('configs')
    checks = 0
    skipped = 0
    stopped_early = False
    try:
        workload = build_workload(workdir)
        for mode in modes:
            print('=== %s ===' % mode)
            for c in build_cases(mode):
                label, names, expect_ok, note = (c['label'], c['names'],
                                                 c['expect_ok'], c['note'])
                # A configuration can be fine in fast-forward and broken once a
                # timing model is attached; that is the whole reason this suite
                # runs both.
                mode_note = None
                if mode == 'detailed':
                    for n in names:
                        if n in EXPECT_FAIL_DETAILED:
                            mode_note = EXPECT_FAIL_DETAILED[n]
                            break

                if c['requires'] and c['requires'] not in features:
                    print('%-34s SKIP: build has no %s' % (label, c['requires']))
                    skipped += 1
                    continue

                for ncores in c['cores']:
                    full = '%s -n %d' % (label, ncores)
                    checks += 1
                    limit = args.timeout
                    if mode == 'detailed':
                        limit = c['timeout_detailed'] or args.timeout_detailed
                    core_note = (c['fail_cores'].get(ncores)
                                 if mode == 'detailed' else None)
                    flaky_note = (c['flaky_cores'].get(ncores)
                                  if mode == 'detailed' else None)
                    want_ok = expect_ok and mode_note is None and core_note is None
                    why = note if not expect_ok else (mode_note or core_note)

                    # The case name goes out BEFORE the case runs, unterminated
                    # and flushed, and the verdict finishes the line.
                    #
                    # This is the heartbeat: whatever is running is always the
                    # last thing in the log, so a stuck case shows up as a
                    # dangling line naming itself rather than as silence.  This
                    # sweep is 318 checks over more than an hour, and until now
                    # the only way to tell a slow case from a hung one was to
                    # watch PIDs turn over in a process table.
                    print('%-34s ' % full, end='')
                    sys.stdout.flush()

                    started = time.time()
                    ok, detail = run_config(names, ncores, workdir, workload,
                                            limit, mode, c['output'],
                                            c['counters'], c['app'])
                    took = time.time() - started
                    if flaky_note and want_ok:
                        log.record('%s [%s]' % (full, mode), took, 'PASS')
                        print('PASS (known intermittent, %s this time: %s) [%ds]'
                              % ('ran' if ok else 'failed', flaky_note, round(took)))
                        continue
                    log.record('%s [%s]' % (full, mode), took,
                               'PASS' if ok == want_ok else 'FAIL')
                    if ok == want_ok:
                        status = 'PASS' if want_ok else 'PASS (known: %s)' % why
                        if want_ok and detail != 'ok':
                            status += ' (%s)' % detail
                    elif want_ok:
                        status = 'FAIL: ' + detail
                        failures.append('%s [%s]' % (full, mode))
                    else:
                        status = 'FAIL: now runs -- move it out of EXPECT_FAIL'
                        failures.append('%s [%s]' % (full, mode))
                    print('%s [%ds]' % (status, round(took)))
                    if args.failfast and ok != want_ok:
                        raise FailFast()
            print()
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
    if unclassified:
        print('unclassified configs (add to EXPECT_PASS or EXPECT_FAIL): %s'
              % ', '.join(unclassified))
        failures.append('classification')
    if failures:
        print('%d of %d checks FAILED' % (len(failures), checks))
        return 1
    print('all %d checks passed%s'
          % (checks, ' (%d cases skipped)' % skipped if skipped else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
