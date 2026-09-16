#!/usr/bin/env python3
"""Virtuoso v2 integration suite: prove that additions changed nothing else.

The Virtuoso v2 port (TRAIL, Revelator, Victima, MimicOS v2) lands as a long
series of steps, most of which are supposed to be invisible to every existing
configuration -- new files nothing selects, or subsystems reachable only by
naming them in a config.  "Supposed to be" is the problem: the way that goes
wrong is a shared header or a hot path picking up a change nobody meant to make,
and the symptom is a number moving in a configuration nobody was looking at.

So the suite has two halves:

  --invariance   run the existing smoke configs and require their statistics to
                 be *identical* to a recorded baseline.  This is the safety
                 argument for landing orthogonal work first, and it runs after
                 every step.

  --feature      per-feature checks, added by the step that makes each feature
                 runnable.  Each asserts on performance counters the feature is
                 specified to move, which is enough to tell "wired up and
                 working" from "compiles and does nothing".

Determinism, and how much of it there is
----------------------------------------
Sniper is very nearly reproducible for a fixed build, config and workload, and
the residual noise was measured rather than assumed.  With ASLR on, two runs of
`baseline` differ in 257 of 2346 statistics -- the address-sensitive ones
(access-mru, data-reuse), so every run here goes through `setarch -R`.

With ASLR pinned, five identical runs moved 6 counters out of 2603.  Four are
host wall-clock readings and are dropped outright (VOLATILE).  The other two are
`L2.store-overlapping-misses{,-data}`, which moved by exactly 1 on a value of
19 -- a genuine simulation counter that is simply not stable to the last count.

So the comparison is numeric with a tolerance rather than exact: a counter
passes when it is within TOL_ABS counts or TOL_REL of the baseline.  The
defaults sit just above the measured floor, which keeps the check able to notice
a real change (an unintended behaviour change moves many counters, or moves one
by far more than a count) without failing on the one that will not sit still.

The child's environment is fixed too, and for the same reason.  The guest's
stack sits above its environment block, so anything that changes that block
moves every address below it: adding the three variables `make` exports
(MAKELEVEL, MAKEFLAGS, MFLAGS) moves 108 counters here.  Baselines blessed from
a shell would then never match a run under `make regress`, which is the way the
regression actually invokes this.  So run_sniper() passes an explicit
environment built from a fixed whitelist rather than inheriting one, and the
baseline records a digest of it -- a mismatch is then reported as a mismatch
rather than looking like a behaviour change.

The workload is built at a fixed path, and that matters more than it looks.
With ASLR off the guest's addresses are determined by its environment, and the
program path is part of that: the same binary run from two paths of different
lengths differs in 303 of 2342 counters here.  Compiling it into a fresh
temporary directory per run -- which this suite did at first -- therefore
produced a different answer every invocation and made the comparison
meaningless.

Baselines are local, not committed: they depend on the host toolchain (the
workload is compiled here) so a checked-in copy would be noise on any other
machine.  Bless once at the start of a series, check after every step:

    testing/test_virtuoso.py --bless          # at the base commit
    ...apply a step...
    testing/test_virtuoso.py                  # must be clean
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SNIPER_ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(SNIPER_ROOT, 'tools'))

from common.failfast import FailFast, add_argument as add_failfast
from common.runlog import RunLog

WORKLOAD_SRC = os.path.join(HERE, 'apps', 'workload.c')
WORKLOAD_OUTPUT = re.compile(r'workload isum=1419064784 fsum=100638720\.000')

# Where blessed baselines live.  Gitignored: see the module docstring.
GOLDEN_DIR = os.path.join(HERE, 'goldens', 'virtuoso')

# Host measurements, not simulation results.  These differ run to run by
# construction and say nothing about whether behaviour changed.
VOLATILE = ('roi.ipscore', 'roi.ipstotal', 'roi.walltime', 'time.walltime')

# Measured noise floor is 1 count on one counter; these sit just above it.
TOL_ABS = 2          # counts; absorbs the L2.store-overlapping-misses jitter
TOL_REL = 0.001      # 0.1%; every other counter was stable to the last digit

# The configurations that must not move.  Deliberately the plain ones: none of
# them exercises anything Virtuoso adds, which is exactly why they are the
# canary for a change leaking out of a new subsystem.
INVARIANCE = [
    ('baseline',            ['address_translation_schemes/baseline'],            1),
    ('parametric_baseline', ['address_translation_schemes/parametric_baseline'], 1),
    ('userspace_baseline',  ['address_translation_schemes/userspace_baseline'],  1),
    ('parametric_4c',       ['address_translation_schemes/multicore/'
                             'parametric_baseline_4c'],                          4),
]



def counter(stats, name):
    """A statistic as a plain int, summed over cores.  0 when absent."""
    v = stats.get(name)
    if v is None:
        return 0
    if isinstance(v, (list, tuple)):
        return sum(int(x) for x in v)
    return int(v)


def require_nonzero(*names):
    """Check builder: every named counter must exist and be non-zero."""
    def check(stats):
        missing = [n for n in names if n not in stats]
        if missing:
            return False, 'counter not registered: %s' % ', '.join(missing)
        zero = [n for n in names if counter(stats, n) == 0]
        if zero:
            return False, 'counter stayed zero: %s' % ', '.join(zero)
        return True, ', '.join('%s=%d' % (n, counter(stats, n)) for n in names)
    return check


# Feature checks.  Each entry is (config list, ncores, check) where check takes
# the statistics dict and returns (ok, detail).  Populated by the integration
# steps as they land; see VIRTUOSO_V2_INTEGRATION.md.
FEATURES = {
    # O5: the detailed DRAM model's per-stage queueing breakdown.  baseline
    # drives real DRAM traffic, so every stage should accumulate; a stage stuck
    # at zero means its accumulation site was missed.
    'dram_queueing': (
        ['address_translation_schemes/baseline'], 1,
        require_nonzero('ddr.total-bank-pending-wait-data',
                        'ddr.total-rank-avail-delay-data',
                        'ddr.total-bank-group-avail-delay-data',
                        'ddr.total-dram-bus-queue-delay-data')),

    # O5: the radix page table counts prefetch-walk faults apart from demand
    # faults.  Nothing prefetches yet, so the counter must exist and read zero
    # while demand faults keep being counted -- which is what tells "registered"
    # apart from "absent".
    # F2: Revelator must actually speculate, not silently soft-disable itself.
    # Its constructor disables the engine when the allocator is not a Revelator
    # one, and a disabled engine still runs, still produces a plausible IPC and
    # still registers every counter -- reading zero.  So the check is that the
    # engine issued prefetches and that the allocator's hash placement was used.
    'revelator': (
        ['address_translation_schemes/revelator'], 1,
        require_nonzero('revelator.total_prefetches',
                        'revelator_allocator.hash1_tries',
                        'revelator_allocator.hash1_hits')),

    # F5: Victima probes the cache for leaf PTE lines.  leaf_hits is the
    # discriminating counter: MMUCacheInterface::isLinePresent has a default
    # implementation returning false, so an incorrectly wired Victima still
    # probes -- and misses every single time.  Requiring hits as well as probes
    # is what separates "the probe is asking" from "the cache is answering".
    # I10: TRAIL, the temporal PTE prefetcher.  The chain has to run end to end --
    # observe a demand access, learn a delta from it, turn the learned deltas
    # into a prediction, and prefetch pages off that prediction.  Asserting only
    # that it was constructed would pass for a prefetcher that learns nothing,
    # and asserting only on learning would pass for one that never predicts.
    'trail': (
        ['address_translation_schemes/temporal_pte_stress'], 1,
        require_nonzero('tlb_temporal_pte.demand_accesses',
                        'tlb_temporal_pte.learning_inserts',
                        'tlb_temporal_pte.predictions_issued',
                        'tlb_temporal_pte.depth0_predictions',
                        'tlb_temporal_pte.depth0_pages_prefetched',
                        'tlb_temporal_pte.prefetch_successful',
                        'tlb_temporal_pte.demand_hit_inflight')),

    # I10: Berti, the stride-learning TLB prefetcher.  Learn a stride, turn it
    # into a prediction, and issue the prefetch.  Unlike TRAIL this activates at
    # the default TLB geometry, because Berti learns over 256KB regions and the
    # workload's page footprint does not depend on TLB size -- shrinking the
    # TLBs moves its counters not at all.
    # I11: the Agile TLB Prefetcher.  ATP arbitrates between three child
    # prefetchers, so the check has to see a child actually chosen and a real
    # prefetch walk issued -- not just the demand walks it observes.  It needs
    # the stress geometry: at the default one its 8-bit enable counter
    # oscillates across 112 queries and never crosses the threshold, leaving
    # every query recorded as atp_choice_disable.
    # K1: the eight reserved magic commands.  MagicServer's switch ends in a
    # default: that asserts on an unrecognised command, so before K1 a guest
    # built against the new sim_api.h took the simulator down -- "Got invalid
    # Magic 27" -- rather than running.  The guest issues all eight and must
    # come out the other side.
    #
    # It also issues SIM_CMD_THREAD_ID, which is handled rather than stubbed.
    # That is what distinguishes a magic path that is live and returning 0
    # from the stubs, from one that is not reaching MagicServer at all: a
    # stub-only check would pass just as well if every magic instruction were
    # being dropped on the floor.
    # K2: the per-thread ROI barrier.  SimRoiStart cannot express "start
    # measuring once all my threads are ready" -- whichever thread arrives
    # first opens ROI.  The barrier holds it shut until the last expected
    # joiner arrives.
    #
    # The guest runs 4M iterations with only one of two expected threads
    # joined, then 20k with both.  Asserting that the models enabled once is
    # not enough: a barrier that opened on the first join would also enable
    # once.  What separates the two is how much was measured -- ~136k
    # instructions when the barrier gates, ~32.5M when it does not -- so the
    # instruction count is the assertion and the enable count rides along.
    #
    # Needs general/magic: under the default ROI_FULL the simulator measures
    # the whole application and every ROI magic command is accepted and
    # ignored, so this check would pass without the barrier working at all.
    'roi_barrier': (
        ['address_translation_schemes/roi_barrier'], 2,
        lambda stats: (
            stats['__stdout__'].count('Enabling performance models') == 1
            and stats['__stdout__'].count('Disabling performance models') == 1
            and counter(stats, 'performance_model.instruction_count') < 1000000,
            'measured %d instructions (phase 1 alone is ~32.5M, so the '
            'barrier held); enabled once, disabled once'
            % counter(stats, 'performance_model.instruction_count')),
        ('roi_barrier.c', r'roi_barrier done sink=')),

    # K3: a forked child gets its own SIFT FIFO.  The recorder opens it from
    # forkAfterInChild; Virtuoso drops that call so a child that goes on to
    # execve does not strand a header in the FIFO, but nothing would then open
    # it for a child that does not exec -- threads.cc opens only for
    # threadid > 0 -- and that child would record nothing at all.  A child
    # recording into nothing is silent, so the check is that both processes
    # produced output and both traces finished.
    'fork_child': (
        ['address_translation_schemes/baseline'], 2,
        lambda stats: (
            'fork_child parent sum=' in stats['__stdout__']
            and 'fork_child child sum=' in stats['__stdout__']
            and stats['__stdout__'].count('-- DONE --') == 2,
            'parent and child both recorded; %d traces finished'
            % stats['__stdout__'].count('-- DONE --')),
        ('fork_child.c', r'fork_child parent sum=')),

    # K3: the recorder takes its app_id from SIFT_CHILD_APP_ID when the
    # environment carries one.  forkAfterInChild sets it so a followed child
    # keeps the app_id Sniper allocated instead of reusing the parent's and
    # writing over the parent's FIFOs -- the recorder's main() runs again
    # after an execve with a fresh address space, so the variable is the only
    # thing that survives.
    #
    # The value is 0, which is what the knob would have given anyway: the
    # point is to prove the branch executes, without moving the run off the
    # FIFO names Sniper is waiting on.
    'sift_child_app_id': (
        ['address_translation_schemes/baseline'], 1,
        lambda stats: (
            'Using app_id=0 from SIFT_CHILD_APP_ID env' in stats['__stdout__'],
            'recorder took its app_id from the environment'),
        ('workload.c', r'workload isum=', {'SIFT_CHILD_APP_ID': '0'})),

    # K4: SimReceiveMessage reads a per-core event queue rather than a single
    # shared MimicOSMessage slot.  With one slot a core got the last event
    # posted on any core, and whatever was left in it when nothing had been
    # posted at all.
    #
    # This checks the consumer, not the queueing.  Nothing posts events yet --
    # every producer (new_thread, thread_exit, quantum_expired, and the
    # page-fault path) sits behind the userspace kernel -- so FIFO order and
    # per-core isolation cannot be observed from a guest until K5.  What is
    # observable is that the handler runs and answers definitely: the guest
    # writes a sentinel into argc and the handler must overwrite it with 0.
    # A handler that never ran, or returned without writing, leaves the
    # sentinel and this fails.
    'mimicos_event_queue': (
        ['address_translation_schemes/userspace_baseline'], 1,
        lambda stats: (
            'mimicos_event first argc=0' in stats['__stdout__']
            and 'mimicos_event second argc=0' in stats['__stdout__']
            and '1515870810' not in stats['__stdout__'],   # 0x5A5A5A5A
            'empty queue reported as argc=0 twice, sentinel overwritten'),
        ('mimicos_event.c', r'mimicos_event done')),

    # K5a: the AppReaderPool, and the magic commands that read it.  Readers
    # used to be reachable only as the app reader of the TraceThread that
    # created them, so one injected application could exist and only its
    # creator could run it.  The pool makes every reader addressable by id.
    #
    # SimGetInjectedAppId must read -1 before anything is injected and the
    # registered id afterwards -- that is how a kernel learns what it may
    # schedule.  SimContextSwitchTo is exercised only through its miss path:
    # on a hit it returns 42, which suppresses the SIFT response record so the
    # caller is never resumed, making it a kernel-only command.  On a miss it
    # has to answer normally, and the guest surviving the miss is the
    # assertion -- with Virtuoso's 42 there, the caller blocks forever.
    'app_reader_pool': (
        ['address_translation_schemes/userspace_baseline'], 2,
        lambda stats: (
            'pool before_inject atid=-1' in stats['__stdout__']
            and 'pool after_inject atid=2' in stats['__stdout__']
            and 'Registered injected reader as app_thread_id 2' in stats['__stdout__']
            and 'no reader in pool' in stats['__stdout__']
            and 'pool survived_miss' in stats['__stdout__'],
            'atid -1 before inject and 2 after; miss answered without '
            'stranding the caller'),
        ('app_reader_pool.c', r'pool done s=')),

    # K6: fast-fault replay, and the magic ABI underneath it.  The guest
    # passes a seven-word payload by address and the simulator must read back
    # exactly those words, so the check asserts the values rather than that
    # the call survived.
    #
    # That distinction is the whole point.  Two defects in the magic macros
    # were invisible to every check that only looked at survival: the result
    # register was not earlyclobber, so the payload pointer could be
    # allocated to the register the asm overwrites first and the simulator
    # read from the command number instead of the pointer; and there was no
    # "memory" clobber, so a local payload's stores were dead code.  Both
    # delivered a well-formed call carrying nothing.
    'fast_fault': (
        ['address_translation_schemes/baseline'], 1,
        lambda stats: (
            'fp_id=42' in stats['__stdout__']
            and 'mean_cyc=5000' in stats['__stdout__']
            and 'frame_pa=0xcafe000' in stats['__stdout__']
            and 'trace_len=4' in stats['__stdout__']
            and 'cursor=7' in stats['__stdout__']
            and 'SetFastFaultCharge] cycles=1234' in stats['__stdout__'],
            'payload arrived intact (fp_id, mean_cycles, frame_pa, '
            'trace_len, cursor) and the per-fault charge was set'),
        ('fast_fault.c', r'fast_fault done s=')),

    'magic_reserved': (
        ['address_translation_schemes/baseline'], 1,
        lambda stats: (
            'magic_k1 survived all eight reserved commands' in stats['__stdout__']
            and 'magic_k1 thread_id=0' in stats['__stdout__']
            and 'invalid Magic' not in stats['__stdout__'],
            'all eight reserved commands returned; thread_id handled'),
        ('magic_k1.c', r'magic_k1 survived all eight reserved commands')),

    'atp': (
        ['address_translation_schemes/atp_stress'], 1,
        require_nonzero('tlb_atp.queries',
                        'tlb_atp.demand_walks_successful',
                        'tlb_atp.atp_choice_stp',
                        'tlb_atp.real_prefetch_candidates_stp',
                        'tlb_atp.real_prefetch_walks_issued',
                        'tlb_atp.real_prefetch_successful')),

    # I11: the Recency TLB prefetcher.  This one needs the stress geometry for
    # a different reason than the rest: it builds its list out of TLB victims,
    # and the default geometry never evicts anything over a smoke workload, so
    # the list stays empty and it cannot prefetch however many misses it sees.
    # The check therefore covers the whole chain -- victim, push, prediction,
    # prefetch -- and all three neighbour directions.
    'recency': (
        ['address_translation_schemes/recency_stress'], 1,
        require_nonzero('recency_tlb.queries',
                        'recency_tlb.recency_victim_insertions',
                        'recency_tlb.recency_pushes',
                        'recency_tlb.predictions_same_issued',
                        'recency_tlb.predictions_plus1_issued',
                        'recency_tlb.predictions_minus1_issued',
                        'recency_tlb.prefetch_successful',
                        'recency_tlb.pointer_chase_accesses')),

    # I11: the Distance TLB prefetcher.  Unlike atp and recency this activates
    # at the default geometry, so no stress scheme is added for it.
    'dp': (
        ['address_translation_schemes/baseline_dp'], 1,
        require_nonzero('dp_tlb.queries',
                        'dp_tlb.distance_computed',
                        'dp_tlb.table_hits',
                        'dp_tlb.predictions_issued',
                        'dp_tlb.prefetch_successful')),

    'berti': (
        ['address_translation_schemes/baseline_berti'], 1,
        require_nonzero('tlb_berti.strides_learned',
                        'tlb_berti.predictions_from_current',
                        'tlb_berti.prefetch_attempts',
                        'tlb_berti.successful_prefetches')),

    'victima': (
        ['address_translation_schemes/victima'], 1,
        require_nonzero('mmu.victima_leaf_probes', 'mmu.victima_leaf_hits')),

    # F6: page faults handled in hardware rather than by the OS.  The split is
    # the whole point of the design, so the check is that faults were taken and
    # that hardware took them -- the same workload routes all 91 to the OS under
    # baseline.
    'hw_fault': (
        ['address_translation_schemes/hw_fault'], 1,
        lambda stats: (
            counter(stats, 'mmu.page_faults_hw_handled') > 0
            and counter(stats, 'mmu.total_hw_fault_latency') > 0,
            'hw=%d os=%d latency=%d'
            % (counter(stats, 'mmu.page_faults_hw_handled'),
               counter(stats, 'mmu.page_faults_os_handled'),
               counter(stats, 'mmu.total_hw_fault_latency')))),

    # F7: the Linux daemon models run from HOOK_PERIODIC at a simulated
    # interval.  Their default interval is 10ms and a smoke workload finishes in
    # well under one, so an unconfigured daemon registers every counter and
    # never scans -- which is why the scheme shortens the interval and why the
    # check is on scans rather than on the counters merely existing.
    #
    # kswapd is deliberately not asserted on: nothing here exposes a free-page
    # count to bind it to, so it scans without reclaiming.  See mimicos.cc.
    # I2: a TLB whose set count is not a power of two.  CacheBase asserts
    # "Caches of non-power of 2 size need funky hash function" when such a cache
    # uses HASH_MASK, so before the TLBs were switched to HASH_MOD this
    # configuration aborted with SIGABRT during startup.  The check is simply
    # that it runs and the TLB is used -- every other configuration here has a
    # power-of-two set count, where the two hashes are the same expression and
    # the difference cannot be seen.
    # I3/I4: the MMU prefetch path, under enough TLB pressure to use it.  The
    # default TLB geometry takes 91 misses in 1.9M instructions, at which point
    # every prefetch counter reads zero and a change here is unobservable.
    #
    # The assertion is an invariant rather than a value: sub-line prefetch
    # credit can never exceed the number of lines actually prefetched, because
    # credit is only ever granted on a line that was marked.  It held at 0 <= 0
    # while nothing marked lines, and it keeps holding once the MMU starts
    # tagging them.  It is what catches m_prefetch_pending going uninitialised:
    # heap garbage credited 171 hits against 0 prefetches.
    'tlb_prefetch_stress': (
        ['address_translation_schemes/tlb_prefetch_stress'], 1,
        lambda stats: (
            counter(stats, 'L2.prefetches') > 0
            and counter(stats, 'L2.hits-prefetch-subline') > 0
            # Eight 8-byte PTEs share a 64-byte line, so one prefetched line can
            # be credited at most eight times -- and exactly zero times if it was
            # never prefetched, which is what catches an uninitialised
            # m_prefetch_pending crediting heap garbage.
            and (counter(stats, 'L2.hits-prefetch-subline')
                 <= 8 * counter(stats, 'L2.prefetches')),
            'tlb_misses=%d walks=%d prefetches=%d line_credit=%d subline_credit=%d'
            % (counter(stats, 'mmu.tlb_misses_data'),
               counter(stats, 'radix_4level.page_table_walks'),
               counter(stats, 'L2.prefetches'),
               counter(stats, 'L2.hits-prefetch'),
               counter(stats, 'L2.hits-prefetch-subline')))),

    # I6: an L2 TLB hit has to cost something.  The only branch handling
    # hit_level 1 was guarded on page-size prediction being enabled, and every
    # configuration here has it disabled, so L2 hits were free and
    # mmu.tlb_latency_1 was exactly zero.  Asserting it is non-zero while L2 hits
    # happen is the regression test; the level-0 latency is checked alongside so
    # that a config which simply stopped using the TLB cannot pass.
    'l2_tlb_latency': (
        ['address_translation_schemes/baseline'], 1,
        lambda stats: (
            counter(stats, 'mmu.tlb_hits_data') > 0
            and counter(stats, 'mmu.tlb_latency_0') > 0
            and counter(stats, 'mmu.tlb_latency_1') > 0,
            'hits=%d l1_latency=%d l2_latency=%d'
            % (counter(stats, 'mmu.tlb_hits_data'),
               counter(stats, 'mmu.tlb_latency_0'),
               counter(stats, 'mmu.tlb_latency_1')))),

    'nonpow2_tlb': (
        ['address_translation_schemes/nonpow2_tlb'], 1,
        require_nonzero('mmu.tlb_hits_data', 'mmu.tlb_misses_data')),

    'os_daemons': (
        ['address_translation_schemes/os_daemons'], 1,
        require_nonzero('kcompactd.scans', 'khugepaged.scans')),

    'radix_fault_split': (
        ['address_translation_schemes/baseline'], 1,
        lambda stats: (
            ('radix_4level.prefetch_page_faults' in stats
             and counter(stats, 'radix_4level.prefetch_page_faults') == 0
             and counter(stats, 'radix_4level.page_faults') > 0),
            'demand=%d prefetch=%d' % (counter(stats, 'radix_4level.page_faults'),
                                       counter(stats, 'radix_4level.prefetch_page_faults'))
            if 'radix_4level.prefetch_page_faults' in stats
            else 'prefetch_page_faults not registered')),
}


# Fixed, so that the guest's environment -- and therefore its addresses -- are
# the same on every run.  See the module docstring.
WORKLOAD_BIN = os.path.join(GOLDEN_DIR, 'workload')


def build_app(src_name, out_name):
    """Compile one testing/apps/*.c into the golden directory.

    Feature checks that need a guest doing something specific -- issuing a
    magic instruction, say -- build their own binary here rather than reusing
    the shared workload.  The path is fixed for the same reason WORKLOAD_BIN
    is: the guest's program path length shifts its address layout, and a
    mkdtemp() path would move counters between runs.
    """
    src = os.path.join(HERE, 'apps', src_name)
    out = os.path.join(GOLDEN_DIR, out_name)
    if (not os.path.exists(out)
            or os.path.getmtime(out) < os.path.getmtime(src)):
        cc = os.environ.get('CC', 'gcc')
        # -pthread unconditionally: harmless for a single-threaded guest, and
        # required by any that spawns threads to reach the barrier.
        subprocess.run([cc, '-O1', '-pthread',
                        '-I', os.path.join(SNIPER_ROOT, 'include'),
                        '-o', out, src], check=True)
    return out


def build_workload():
    os.makedirs(GOLDEN_DIR, exist_ok=True)
    if (not os.path.exists(WORKLOAD_BIN)
            or os.path.getmtime(WORKLOAD_BIN) < os.path.getmtime(WORKLOAD_SRC)):
        cc = os.environ.get('CC', 'gcc')
        subprocess.run([cc, '-O1', '-o', WORKLOAD_BIN, WORKLOAD_SRC], check=True)
    return WORKLOAD_BIN


def have_setarch():
    return shutil.which('setarch') is not None


# Variables the child is allowed to see.  Everything else is dropped, so the
# result does not depend on who invoked the suite.  See the module docstring.
ENV_KEEP = ('PATH', 'HOME', 'USER', 'LANG', 'LC_ALL', 'TMPDIR', 'SHELL',
            'CC', 'CXX', 'PYTHONPATH')


def child_env():
    env = {k: os.environ[k] for k in ENV_KEEP if k in os.environ}
    # SNIPER_* selects the build and the dependency cache, so it has to survive.
    env.update({k: v for k, v in os.environ.items() if k.startswith('SNIPER_')})
    # Not this one: it points at the regression run's timing log, and its value
    # changes per invocation.
    env.pop('SNIPER_REGRESS_LOG', None)
    return env


def run_env(extra=None):
    """child_env() plus anything a feature check needs the guest to see.

    Only feature checks pass extra: the invariance runs are compared against
    goldens blessed under a recorded digest of child_env(), so adding to the
    environment there would change the guest's address layout and move
    counters.
    """
    env = child_env()
    if extra:
        env.update(extra)
    return env


def env_digest():
    e = child_env()
    return hashlib.sha1(
        '\n'.join('%s=%s' % kv for kv in sorted(e.items())).encode()
    ).hexdigest()[:12]


def run_sniper(names, ncores, outdir, workload, timeout, expect=WORKLOAD_OUTPUT,
               extra_env=None):
    """One simulation, with ASLR pinned so the statistics are reproducible."""
    shutil.rmtree(outdir, ignore_errors=True)
    cmd = []
    if have_setarch():
        cmd += ['setarch', '-R']
    cmd += [os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', str(ncores)]
    cmd += ['-c' + n for n in names]
    cmd += ['-d', outdir, '--', workload]
    try:
        p = subprocess.run(cmd, timeout=timeout, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, cwd=SNIPER_ROOT,
                           env=run_env(extra_env), start_new_session=True)
    except subprocess.TimeoutExpired:
        return None, 'timed out after %ds' % timeout
    if p.returncode != 0:
        err = re.search(r'\*ERROR\*\s*(.{0,60})', p.stdout)
        return None, err.group(1).strip() if err else 'exit %d' % p.returncode
    # run-sniper exits 0 even when the simulator refuses to start.
    if not expect.search(p.stdout):
        return None, 'exit 0 but the workload produced no output'
    return p.stdout, None


def read_stats(outdir):
    import sniper_lib
    return sniper_lib.get_results(resultsdir=outdir)['results']


def comparable(stats):
    """The part of a statistics dict that is a property of the simulation."""
    return {k: v for k, v in stats.items() if k not in VOLATILE}


def golden_path(name):
    return os.path.join(GOLDEN_DIR, name + '.json')


def bless(name, stats, workload):
    os.makedirs(GOLDEN_DIR, exist_ok=True)
    rec = {
        'stats': {k: repr(v) for k, v in sorted(comparable(stats).items())},
        'blessed_at': time.strftime('%Y-%m-%d %H:%M:%S'),
        'commit': git_head(),
        'workload_sha1': file_sha1(workload),
        'env_digest': env_digest(),
    }
    with open(golden_path(name), 'w') as f:
        json.dump(rec, f, indent=1, sort_keys=True)
    return len(rec['stats'])


def as_numbers(v):
    """A statistic as a list of floats, or None if it is not numeric."""
    seq = v if isinstance(v, (list, tuple)) else [v]
    out = []
    for x in seq:
        try:
            out.append(float(x))
        except (TypeError, ValueError):
            return None
    return out


def deviation(want, got):
    """How far one counter moved, as (absolute, relative) worst over cores.

    Returns None when the two are not comparable at all -- a different number of
    cores, or a value that is not a number -- which is treated as a change.
    """
    a, b = as_numbers(want), as_numbers(got)
    if a is None or b is None or len(a) != len(b):
        return None
    worst = (0.0, 0.0)
    for x, y in zip(a, b):
        d = abs(y - x)
        r = d / abs(x) if x else (0.0 if d == 0 else float('inf'))
        if (d, r) > worst:
            worst = (d, r)
    return worst


def compare(name, stats, tol_abs=TOL_ABS, tol_rel=TOL_REL):
    """Compare against the blessed baseline, within tolerance."""
    path = golden_path(name)
    if not os.path.exists(path):
        return None, 'no baseline (run --bless at the base commit)'
    rec = json.load(open(path))
    if rec.get('env_digest') not in (None, env_digest()):
        return None, ('baseline was blessed under a different environment '
                      '(%s, now %s); re-bless' % (rec['env_digest'], env_digest()))
    want = rec['stats']
    got = {k: repr(v) for k, v in comparable(stats).items()}

    added = sorted(set(got) - set(want))
    removed = sorted(set(want) - set(got))

    outside = []          # (abs, rel, key) beyond tolerance
    inside = 0            # moved, but within tolerance
    for k in sorted(set(got) & set(want)):
        if got[k] == want[k]:
            continue
        dev = deviation(_unrepr(want[k]), _unrepr(got[k]))
        if dev is None:
            outside.append((float('inf'), float('inf'), k))
            continue
        d, r = dev
        if d > tol_abs and r > tol_rel:
            outside.append((d, r, k))
        else:
            inside += 1

    if outside or removed:
        bits = []
        if outside:
            outside.sort(reverse=True)
            d, r, k = outside[0]
            bits.append('%d counters outside tolerance (worst %s: %s -> %s, '
                        '%+.3g = %.2g%%)'
                        % (len(outside), k, want[k][:18], got[k][:18], d, 100 * r))
        if removed:
            bits.append('%d counters disappeared (e.g. %s)'
                        % (len(removed), removed[0]))
        return False, '; '.join(bits)

    note = '%d counters' % len(got)
    if inside:
        note += ', %d jittered within tolerance' % inside
    if added:
        note += ', %d new' % len(added)
    return True, note


def _unrepr(s):
    """Recover a value from the repr() stored in a baseline."""
    try:
        import ast
        return ast.literal_eval(s)
    except (ValueError, SyntaxError):
        return s


def git_head():
    try:
        return subprocess.run(['git', 'rev-parse', '--short', 'HEAD'],
                              cwd=SNIPER_ROOT, capture_output=True, text=True,
                              check=True).stdout.strip()
    except Exception:
        return '?'


def file_sha1(path):
    return hashlib.sha1(open(path, 'rb').read()).hexdigest()[:12]


def main():
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass

    ap = argparse.ArgumentParser()
    ap.add_argument('--bless', action='store_true',
                    help='record the current statistics as the baseline')
    ap.add_argument('--feature', action='append',
                    help='run only this feature check (repeatable)')
    ap.add_argument('--only', action='append',
                    help='run only this invariance config (repeatable)')
    ap.add_argument('--timeout', type=int, default=600)
    ap.add_argument('--tol-abs', type=float, default=TOL_ABS,
                    help='counters may differ by this many counts (default %d)' % TOL_ABS)
    ap.add_argument('--tol-rel', type=float, default=TOL_REL,
                    help='...or by this fraction (default %g)' % TOL_REL)
    add_failfast(ap)
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(SNIPER_ROOT, 'lib', 'sniper')):
        print('error: lib/sniper not built; run make first', file=sys.stderr)
        return 1
    if not have_setarch():
        print('warning: setarch not found; ASLR stays on and statistics will '
              'not be reproducible', file=sys.stderr)

    # One run at a time.  Two instances share the baselines and the workload
    # binary, and a --bless overlapping a build or a check blesses a mixture of
    # two binaries -- which then reads as a behaviour change in whatever step
    # comes next.  This has cost two false positives already, so it is refused
    # rather than documented.
    os.makedirs(GOLDEN_DIR, exist_ok=True)
    lock = os.path.join(GOLDEN_DIR, '.lock')
    try:
        lock_fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        os.write(lock_fd, ('pid %d\n' % os.getpid()).encode())
        os.close(lock_fd)
    except FileExistsError:
        print('error: another run holds %s.\n'
              '       Wait for it, or remove the file if it is stale.' % lock,
              file=sys.stderr)
        return 1

    log = RunLog('virtuoso')
    workdir = tempfile.mkdtemp(prefix='sniper-virtuoso-')
    failures = 0
    stopped_early = False
    try:
        workload = build_workload()

        cases = INVARIANCE
        if args.only:
            cases = [c for c in cases if c[0] in args.only]
        if args.feature:                 # --feature implies feature checks only
            cases = []

        for name, cfgs, ncores in cases:
            print('%-22s ' % name, end='')
            sys.stdout.flush()
            started = time.time()
            outdir = os.path.join(workdir, 'inv-' + name)
            out, err = run_sniper(cfgs, ncores, outdir, workload, args.timeout)
            took = time.time() - started
            if err:
                log.record('invariance:' + name, took, 'FAIL')
                print('FAIL: %s [%ds]' % (err, round(took)))
                failures += 1
                if args.failfast:
                    raise FailFast()
                continue
            stats = read_stats(outdir)
            if args.bless:
                n = bless(name, stats, workload)
                log.record('bless:' + name, took)
                print('blessed %d counters [%ds]' % (n, round(took)))
            else:
                ok, detail = compare(name, stats, args.tol_abs, args.tol_rel)
                log.record('invariance:' + name, took,
                           'PASS' if ok is not False else 'FAIL')
                if ok is None:
                    print('SKIP (%s) [%ds]' % (detail, round(took)))
                elif ok:
                    print('PASS (%s) [%ds]' % (detail, round(took)))
                else:
                    print('FAIL: %s [%ds]' % (detail, round(took)))
                    failures += 1
                    if args.failfast:
                        raise FailFast()
            shutil.rmtree(outdir, ignore_errors=True)

        names = args.feature if args.feature else sorted(FEATURES)
        for fname in names:
            if fname not in FEATURES:
                print('%-22s SKIP (no such feature check)' % fname)
                continue
            entry = FEATURES[fname]
            # A feature may name its own guest binary: (cfgs, ncores, check,
            # (source, output_pattern)).  Checks that only need counters keep
            # the three-element form and run the shared workload.
            if len(entry) == 4:
                cfgs, ncores, check, guest = entry
                # (source, output_pattern) or (source, output_pattern, env)
                src, pattern = guest[0], guest[1]
                extra_env = guest[2] if len(guest) > 2 else None
                app = build_app(src, os.path.splitext(src)[0])
                expect = re.compile(pattern)
            else:
                cfgs, ncores, check = entry
                app, expect = workload, WORKLOAD_OUTPUT
                extra_env = None
            print('%-22s ' % fname, end='')
            sys.stdout.flush()
            started = time.time()
            outdir = os.path.join(workdir, 'feat-' + fname)
            out, err = run_sniper(cfgs, ncores, outdir, app, args.timeout,
                                  expect, extra_env)
            took = time.time() - started
            if err:
                log.record('feature:' + fname, took, 'FAIL')
                print('FAIL: %s [%ds]' % (err, round(took)))
                failures += 1
                if args.failfast:
                    raise FailFast()
                continue
            feat_stats = read_stats(outdir)
            # Checks on frontend behaviour read the guest's output rather than
            # a counter; it rides along under a name no statistic can collide
            # with.  Only feature checks see it -- the invariance goldens are
            # blessed from read_stats() untouched.
            feat_stats['__stdout__'] = out
            ok, detail = check(feat_stats)
            log.record('feature:' + fname, took, 'PASS' if ok else 'FAIL')
            print('%s (%s) [%ds]' % ('PASS' if ok else 'FAIL', detail, round(took)))
            if not ok:
                failures += 1
                if args.failfast:
                    raise FailFast()
            shutil.rmtree(outdir, ignore_errors=True)

    except FailFast:
        stopped_early = True
    finally:
        if stopped_early:
            print('\nstopped at the first failure; work directory kept at %s'
                  % workdir)
        else:
            shutil.rmtree(workdir, ignore_errors=True)
        log.finish()
        try:
            os.unlink(lock)
        except OSError:
            pass

    print()
    ran = len(log.checks)
    if failures:
        print('%d of %d checks FAILED' % (failures, ran))
        return 1
    print('all %d checks passed' % ran)
    return 0


if __name__ == '__main__':
    sys.exit(main())
