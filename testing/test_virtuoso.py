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

# The run stream's line shape, taken from the lnav format so the two agree.
def _stamped_line_regex():
    fmt = os.path.join(SNIPER_ROOT, 'tools', 'lnav', 'formats', 'sniper', 'sniper_run.json')
    pattern = json.load(open(fmt))['sniper_run']['regex']['std']['pattern']
    return re.compile(pattern.replace('(?<', '(?P<'))
STAMPED = _stamped_line_regex()

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



def live_feed_check(stats):
    path = os.path.join(stats['__outdir__'], 'simulation', 'sim.live')
    if not os.path.exists(path):
        return False, 'no sim.live written'
    recs = [json.loads(l) for l in open(path) if l.strip()]
    evs = [r['ev'] for r in recs]
    samples = [r for r in recs if r['ev'] == 'sample']
    busy = [any(c['instrs'] > 0 for c in r['cores']) for r in samples]
    cores_seen = set(i for r in samples for i, c in enumerate(r['cores']) if c['instrs'] > 0)
    stacks = all(sum(c['cpi'].values()) > 0 for r in samples for c in r['cores'] if c['instrs'] > 0)
    parks = sum(1 for r in recs if r['ev'] == 'thread' and r['what'] == 'stall' and r.get('reason') == 'pause')
    quanta = sum(1 for r in recs if r['ev'] == 'os' and r['kind'] == 'quantum_expired')
    switches = sum(1 for r in recs if r['ev'] == 'os' and r['kind'] == 'switch_to')
    exits = sum(1 for r in recs if r['ev'] == 'os' and r['kind'] == 'thread_exit')
    # At most one park: the worker whose application exits first waits on its
    # empty queue while the other finishes, but whether it reaches that wait
    # before the last exit posts shutdown is a scheduling race -- both orders
    # are correct, so 0 or 1 parks are accepted, not a fixed count.
    # Every quantum hands back to the kernel, which switches an application
    # in again: at least one switch per quantum, plus new_thread and the
    # faults the kernel takes.
    ok = (evs[0] == 'header' and 'end' in evs and any(busy) and cores_seen >= {0, 1}
          and stacks and parks <= 1 and quanta >= 100 and switches >= quanta and exits == 2)
    # And the dashboard draws it: one frame, no terminal, with the legend,
    # the core lanes carrying the kernel's events, the thread lanes, totals.
    frame = subprocess.run([os.path.join(SNIPER_ROOT, 'tools', 'sniper-live'), '--once', '--no-color',
                            '-W', '120', '-H', '40', '--log', os.path.join(stats['__outdir__'], 'simulation', 'sim.stdout'),
                            '--log-filter', 'kernel', path], stdout=subprocess.PIPE, text=True)
    drawn = (frame.returncode == 0 and 'CPI stack' in frame.stdout and ' mem ' in frame.stdout
             and 'core 0' in frame.stdout and 'Q' in frame.stdout and 'T0 ' in frame.stdout
             and 'T1 ' in frame.stdout and 'quanta %d' % quanta in frame.stdout
             and 'log: kernel' in frame.stdout and '[kernel] [MimicOS]' in frame.stdout)
    ok = ok and drawn
    return ok, ('%d records, %d samples (cores busy: %s), stacks %s, %d parks, %d quanta, %d switches, %d exits; frame %s'
                % (len(recs), len(samples), sorted(cores_seen), 'nonzero' if stacks else 'EMPTY',
                   parks, quanta, switches, exits, 'drawn' if drawn else 'NOT drawn (rc %d)' % frame.returncode))


def mimicos_daemons_check(stats):
    """The userspace kernel's daemons: each ran, at its simulated interval."""
    out = stats['__stdout__']
    scans = {}
    for name in ('kcompactd', 'khugepaged', 'kswapd'):
        m = re.search(r'\[MimicOS\] %s stopped\. Scans=(\d+)' % name, out)
        scans[name] = int(m.group(1)) if m else -1
    # A daemon waits out its interval in simulated time, so over the
    # application's run it scans elapsed / interval times, give or take the
    # one at either end.  A daemon whose clock stood still would wake at
    # every barrier and scan thousands of times; one that never ran, zero.
    elapsed = as_numbers(stats.get('performance_model.elapsed_time', [0]))
    idle = as_numbers(stats.get('performance_model.idle_elapsed_time', [0]))
    parked = as_numbers(stats.get('performance_model.cpiSyncFutex', [0]))
    interval_fs = 50 * 10**9                              # 50 us
    expected = int(elapsed[0] // interval_fs)
    faults = counter(stats, 'mmu.page_faults')
    ok = (all(0 < n and abs(n - expected) <= 1 for n in scans.values())
          # the three daemon cores spent the run parked in their timed
          # wait, a futex with a deadline on the simulated clock
          and len(parked) >= 4 and all(t > 0 for t in parked[1:4])
          # and the kernel's exit woke them rather than waiting out an
          # interval: the application's core idled for well under one
          and idle[0] < interval_fs / 10
          # the application ran to completion and the kernel reached its exit
          # reports, the daemons having changed nothing in its memory use:
          # the fault count and the mapping check are those of
          # reservethp_32GB.ini without daemons.  (67 until the ip_stride
          # L1-D prefetcher came on in meteor_lake_pcore.cfg: the count is
          # timing-dependent by one fault, and the no-daemons reference
          # measured on that config is 68 with the same 28/28/0 mappings.)
          and '[TRACE:0] -- DONE --' in out
          and 'shutdown event received' in out
          and faults == 68
          and ('Core 0 checked 28 unique VA->PA mappings, 28 unique PA->VA '
               'mappings, 0 violations') in out)
    return ok, ('kcompactd/khugepaged/kswapd scans %d/%d/%d (expected %d +-1 '
                'over %d us), parked per core %s us, core 0 idle %d ns, %d faults'
                % (scans['kcompactd'], scans['khugepaged'], scans['kswapd'],
                   expected, elapsed[0] // 10**9,
                   [int(t // 10**9) for t in parked[:4]], idle[0] // 10**6, faults))


def counter(stats, name):
    """A statistic as a plain int, summed over cores.  0 when absent."""
    v = stats.get(name)
    if v is None:
        return 0
    if isinstance(v, (list, tuple)):
        return sum(int(x) for x in v)
    return int(v)


def require_nonzero(*names, registered=()):
    """Check builder: every named counter must exist and be non-zero.

    `registered` names counters that only have to exist -- a feature's own
    counter that the workload legitimately leaves at zero, where existing is
    still what tells the feature apart from its absence.
    """
    def check(stats):
        missing = [n for n in names + tuple(registered) if n not in stats]
        if missing:
            return False, 'counter not registered: %s' % ', '.join(missing)
        zero = [n for n in names if counter(stats, n) == 0]
        if zero:
            return False, 'counter stayed zero: %s' % ', '.join(zero)
        return True, ', '.join('%s=%d' % (n, counter(stats, n))
                               for n in names + tuple(registered))
    return check


# Feature checks.  Each entry is (config list, ncores, check) where check takes
# the statistics dict and returns (ok, detail).  Populated by the integration
# steps as they land; docs/VIRTUOSO_MIMICOS_STACK.md section 2 lists them.
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
    # F4: the Revelator exception handler places the leaf page-table frame
    # through the allocator's hashed path (allocate with
    # is_pagetable_allocation), which is what the speculative engine relies
    # on; hash1_successful_pt_allocations counts exactly those placements
    # and stays zero under the default handler.
    'revelator': (
        ['address_translation_schemes/revelator'], 1,
        require_nonzero('revelator.total_prefetches',
                        'revelator_allocator.hash1_tries',
                        'revelator_allocator.hash1_hits',
                        'revelator_allocator.hash1_successful_pt_allocations')),

    # The NUMA Revelator: the same hash placement, scoped to a NUMA node.
    # Every counter here is registered only by the NUMA engine and allocator
    # (prefixes numa_revelator / numa_revelator_allocator), so plain revelator
    # reads them as absent.  The engine has to train from PTWs, consult the
    # hint table while the counters are undecided and find a hint there, be
    # confident off the counters at least once, and prefetch on both the data
    # and the page-table side; the allocator has to place frames on the
    # CPU-local node by hash.  The stress scheme (shrunk TLBs) is what makes
    # the data side run at all: see numa_revelator_stress.cfg.
    'numa_revelator': (
        ['address_translation_schemes/numa_revelator_stress'], 1,
        require_nonzero('numa_revelator.total_prefetches',
                        'numa_revelator.prefetches_0',
                        'numa_revelator.prefetches_pt_0',
                        'numa_revelator.node0_predictions',
                        'numa_revelator.train_updates',
                        'numa_revelator.hint_lookups',
                        'numa_revelator.hint_hits',
                        'numa_revelator.counter_confident',
                        'numa_revelator_allocator.hash1_tries',
                        'numa_revelator_allocator.hash1_hits',
                        'numa_revelator_allocator.node0_allocs',
                        'numa_revelator_allocator.local_allocs')),

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
        # The guest's closing line never prints, and that is the barrier
        # working: the last thread to leave stops the TraceManager, so the run
        # ends before the application returns from main.  Phase 1's marker is
        # the last output guaranteed to appear.
        ('roi_barrier.c', r'roi_barrier phase1 done')),

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

    # The per-core event queue, from the consumer's side.  SimReceiveMessage
    # is a wait: an empty queue parks the caller through the ThreadManager
    # (like an idle CPU halted until an interrupt) and a poster wakes it.  The
    # one producer a guest can drive is injection, which posts new_thread to
    # the injecting core's queue; the guest injects and receives, and what
    # arrives is pinned: argc=2, code 3 (NEW_THREAD), the injected app's main
    # thread id encodeId(1, 0) = 65536, sentinel overwritten.  The K4 form of
    # this check expected argc=0 back at once from an empty queue; that was
    # the stopgap before anything posted events.  Parking, the wake on
    # shutdown and the exit reports are covered by mimicos_two_app_smp.
    'mimicos_event_queue': (
        ['address_translation_schemes/userspace_baseline'], 1,
        lambda stats: (
            'mimicos_event injected app_id=1' in stats['__stdout__']
            and 'mimicos_event new_thread argc=2 code=3 arg=65536' in stats['__stdout__']
            and '1515870810' not in stats['__stdout__'],   # 0x5A5A5A5A
            'new_thread delivered as %s'
            % (', '.join(re.findall(r'mimicos_event new_thread (argc=\S+ code=\S+ arg=\S+)',
                                    stats['__stdout__'])) or 'nothing')),
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
            # the first injected application gets app id 1, and its main
            # thread is keyed at encodeId(1, 0) = 65536; a kernel reads the
            # app id and derives the thread id by shifting, so the two must
            # agree or it schedules an id nobody registered
            and 'pool after_inject atid=1' in stats['__stdout__']
            and 'Registered injected reader as app_thread_id 65536' in stats['__stdout__']
            and 'no reader in pool' in stats['__stdout__']
            and 'pool survived_miss' in stats['__stdout__'],
            'app id -1 before inject and 1 after, keyed at 65536; miss '
            'answered without stranding the caller'),
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
            and 'SetFastFaultCharge] cycles=1234' in stats['__stdout__']
            # The cost has to land on the core's timeline, in its own bucket.
            # 10 faults x 1M cycles against a ~200k baseline; anything within
            # 10%% of 10M can only be the charge.
            and abs(counter(stats, 'performance_model.cpiFastFault')
                    * float(stats.get('fs_to_cycles', 0) or 0)
                    - 10000000) < 1000000
            # and not misattributed to the DVFS transition counter
            and counter(stats, 'performance_model.cpiSyncDvfsTransition') == 0,
            'payload arrived intact and %d cycles were charged to '
            'cpiFastFault, cpiSyncDvfsTransition untouched'
            % round(counter(stats, 'performance_model.cpiFastFault')
                    * float(stats.get('fs_to_cycles', 0) or 0))),
        ('fast_fault.c', r'fast_fault done s=')),

    # The linux_buddy_anon allocator's per-CPU page pool, exercised through the
    # in-repo MimicOS kernel.  Selecting an allocator only proves the factory
    # built one; what makes this allocator different from the others is that a
    # fault either hits its per-CPU pool or refills the pool from the buddy in
    # a batch, and that distinction was invisible until the kernel reported
    # which path each fault took.
    #
    # Both paths have to appear.  All-fast would mean the pool never drains and
    # the refill path is dead; all-slow would mean the pool never serves
    # anything and the allocator is a buddy with extra steps.  With a
    # 32-page refill batch, 59 faults come out as 57 hits and 2 refills.
    'mimicos_pcp_allocator': (
        ['address_translation_schemes/userspace_baseline'], 2,
        lambda stats: (
            stats['__stdout__'].count('alloc_path=fast') > 0
            and stats['__stdout__'].count('alloc_path=slow') > 0
            # every fault classified: an unknown here means the allocator
            # stopped reporting and the check would otherwise pass on silence
            and stats['__stdout__'].count('alloc_path=unknown') == 0
            and (stats['__stdout__'].count('alloc_path=fast')
                 > stats['__stdout__'].count('alloc_path=slow'))
            # and the shipped userspace configuration keeps Sniper's own
            # scheduler on a non-migrating type.  MimicOS picks a core's event
            # queue from the posting thread's current core while the kernel
            # waits on a fixed one, so a migration between those points would
            # deliver an event to a core nobody polls.  base_system_userspace
            # sets scheduler/type=static; this fails if that is ever loosened.
            and 'userspace MimicOS with scheduler/type' not in stats['__stdout__'],
            '%d faults: %d served from the per-CPU pool, %d refilled it; '
            'scheduler type is non-migrating'
            % (stats['__stdout__'].count('alloc_path='),
               stats['__stdout__'].count('alloc_path=fast'),
               stats['__stdout__'].count('alloc_path=slow'))),
        ('startup_mimicos', r'alloc_path=', None,
         ['mimicos/configs/linux_buddy_anon_32GB.ini',
          'test/sniper-in-sniper/fft.sift', '/tmp/sniper_pcp_check.txt'])),

    # MimicOS's own scheduler, which is a separate scheduler from Sniper's and
    # configured separately: Sniper's scheduler/type decides which core a
    # thread runs on, this decides which application thread a core runs, and
    # the kernel reads none of the simulator's configuration.  With no
    # [scheduler] section the policy is DISABLED and the kernel falls back to a
    # per-core FIFO runqueue, which is what every other configuration here
    # uses -- so this is the only check that exercises the scheduler at all.
    #
    # The task has to carry the application's real thread id.  The scheduler
    # hands whatever it holds straight to SimContextSwitchTo, so a task
    # registered with the default 0 asks for a thread nobody registered, the
    # application never runs, and the run hangs -- which is exactly what
    # happened once id 0 stopped naming a reader by accident.
    'mimicos_scheduler': (
        ['address_translation_schemes/userspace_baseline'], 2,
        lambda stats: (
            'Scheduler] Configured: policy=rr_simple' in stats['__stdout__']
            and 'Registered app trace as Task' in stats['__stdout__']
            and 'atid=0x10000' in stats['__stdout__']
            and counter(stats, 'trace_thread.quanta_expired') > 0,
            'rr policy configured, task registered at atid 0x10000, '
            '%d quanta expired' % counter(stats, 'trace_thread.quanta_expired')),
        ('startup_mimicos', r'Scheduler\] Configured', None,
         ['mimicos/configs/linux_buddy_anon_sched_rr.ini',
          'test/sniper-in-sniper/fft.sift', '/tmp/sniper_sched_check.txt'])),

    # Two applications on two cores, kernel-driven, end to end.  Each worker
    # injects and runs its own application; the one whose application exits
    # first parks on its empty queue (ThreadManager stall, not a spin), and
    # the last exit wakes it with the synthesized shutdown.  Both workers must
    # report the shutdown, the kernel must exit cleanly (no assertion -- it
    # used to abort on argc=0 from the empty queue) and reach its exit
    # reports, and the round-robin scheduler must have preempted both
    # applications for the whole run.  Pinned at 4 cores like the race test
    # that found the hang; the two spare cores stay idle.
    # The live feed (scripts/live.py): simulated-time samples with the CPI
    # stack and the thread table, thread events, and the software layer's
    # events from HOOK_MIMICOS_EVENT.  On the two-application run both cores
    # retire instructions in the samples, both idle parks show as 'pause'
    # stalls, and the kernel's quanta and switches are all there.
    'live_feed': (
        ['address_translation_schemes/userspace_baseline', '-slive:50000'], 4,
        live_feed_check,
        ('startup_mimicos', r'Scheduler stats: csw=', None,
         ['mimicos/configs/two_app_smp.ini',
          'test/sniper-in-sniper/fft.sift', '/tmp/sniper_live_check.txt'])),

    'mimicos_two_app_smp': (
        ['address_translation_schemes/userspace_baseline'], 4,
        lambda stats: (
            stats['__stdout__'].count('shutdown event received') == 2
            and 'Scheduler stats:' in stats['__stdout__']
            and 'Assertion' not in stats['__stdout__']
            and 'Application has deadlocked' not in stats['__stdout__']
            and counter(stats, 'trace_thread.quanta_expired') >= 100,
            'both workers saw shutdown, kernel exited with its reports, '
            '%d quanta expired' % counter(stats, 'trace_thread.quanta_expired')),
        ('startup_mimicos', r'Scheduler stats: csw=', None,
         ['mimicos/configs/two_app_smp.ini',
          'test/sniper-in-sniper/fft.sift', '/tmp/sniper_two_app_check.txt'])),

    # The userspace kernel's daemons: kswapd, kcompactd and khugepaged as
    # kernel pthreads, each sleeping for its scan interval between scans.
    # Four cores: the static scheduler gives each daemon a core of its own,
    # and with fewer the daemon's pthread_create aborts the run.
    #
    # The daemon's wait has to be a simulated one.  It used to usleep(),
    # which reaches the kernel as clock_nanosleep, and the frontend did not
    # emulate that: the thread slept in host time while the simulator held
    # it running, and as a kernel thread its instructions are never timed,
    # so its core's clock never moved.  The moment the application's core
    # reached the first barrier it waited for the daemon's core, which
    # could not arrive, and the kernel never returned from its first
    # SimContextSwitch.  The check ties the scan counts to the simulated
    # run time, so it fails both when a daemon never runs and when its
    # clock stands still; and it bounds the kernel's idle time, since a
    # stop() that waited out a parked daemon's interval charged the rest
    # of it (10ms by default) to the run.
    'mimicos_daemons': (
        ['address_translation_schemes/userspace_baseline'], 4,
        mimicos_daemons_check,
        ('startup_mimicos', r'kswapd stopped\. Scans=', None,
         ['mimicos/configs/reservethp_daemons_32GB.ini',
          'test/sniper-in-sniper/fft.sift', '/tmp/sniper_daemons_check.txt'])),

    # More software threads than cores.  Eight threads with a barrier between
    # rounds on four cores (the shape of fft -p 8 on -n 4), which deadlocked
    # at the fifth thread: a new thread's recorder blocked in PinCRT's fopen()
    # on its response pipe -- waiting for a trace thread that had no core --
    # holding the stdio lock every other thread's fopen() needs, so no thread
    # could start and the barrier never advanced.  The recorder's FIFO opens
    # no longer wait inside stdio (sift/zfstream.cc), and a trace thread is
    # announced to the scheduler only once its stream is open.
    'oversubscription': (
        ['gainestown'], 4,
        lambda stats: (
            'oversub done threads=8' in stats['__stdout__']
            and counter(stats, 'thread.instruction_count') > 0,
            'eight threads finished on four cores; %d thread instructions'
            % counter(stats, 'thread.instruction_count')),
        ('oversub.c', r'oversub done threads=8')),

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

    # The ip_stride data prefetcher at the L1-D, which meteor_lake_pcore.cfg
    # (and so every scheme) selects as upstream does.  The whole chain: an
    # RPT entry reaches STEADY, generates addresses, and the controller
    # issues them (L1-D.prefetches is its count, zero without a prefetcher).
    # gen_prefetch and L1-D.prefetches are both required because the
    # controller drops a generated address it already holds.
    'ip_stride': (
        ['address_translation_schemes/baseline'], 1,
        require_nonzero('ip_stride.pref_called', 'ip_stride.steady',
                        'ip_stride.gen_prefetch', 'L1-D.prefetches')),

    # The streamer at the L2 (prefetcher_configs/streamer_l2 on baseline):
    # a stream's confidence has to reach the threshold (steady) before it
    # generates, and the L2 controller has to issue what it generates.
    'streamer': (
        ['address_translation_schemes/baseline', 'prefetcher_configs/streamer_l2'], 1,
        require_nonzero('streamer.pref_called', 'streamer.steady',
                        'streamer.gen_prefetch', 'L2.prefetches')),

    # The four ChampSim-derived data prefetchers (port series step 5), each
    # layered on baseline through its fragment.  Each check follows the
    # prefetcher's own chain to the issue: SPP has to hit its signature
    # table and issue; SMS has to see a region twice (ft_hits), find a
    # recorded pattern (pht_hits) and replay it; Berti has to learn a
    # berti (berti_predictions) and issue; IPCP has to classify an IP as
    # a constant stride and issue for it.  prefetches_issued is each
    # prefetcher's count of addresses it handed over, and the controller's
    # count says they were taken.
    'spp': (
        ['address_translation_schemes/baseline', 'prefetcher_configs/spp_l2'], 1,
        require_nonzero('spp.pref_called', 'spp.st_hits', 'spp.prefetches_issued',
                        'L2.prefetches')),
    'sms': (
        ['address_translation_schemes/baseline', 'prefetcher_configs/sms_l1'], 1,
        require_nonzero('sms.pref_called', 'sms.ft_hits', 'sms.pht_hits',
                        'sms.prefetches_generated', 'sms.prefetches_issued',
                        'L1-D.prefetches')),
    'berti_data': (
        ['address_translation_schemes/baseline', 'prefetcher_configs/berti_l1'], 1,
        require_nonzero('berti.pref_called', 'berti.current_pages_hits',
                        'berti.berti_predictions', 'berti.prefetches_issued',
                        'L1-D.prefetches')),
    'ipcp': (
        ['address_translation_schemes/baseline', 'prefetcher_configs/ipcp_l1'], 1,
        require_nonzero('ipcp.pref_called', 'ipcp.cs_prefetches',
                        'ipcp.prefetches_issued', 'L1-D.prefetches')),

    # The ASP TLB prefetcher's lookahead and degree (port series step 6).
    # mmu_base_prefetch.cfg sets lookahead=4 degree=3: once an entry is
    # trained (threshold_reached) ASP walks VPN + (4 + d) * stride for d in
    # 0..2, so every trained access attempts exactly three prefetches --
    # it attempted one, VPN + stride, before, whatever the knobs said.  The
    # ratio is the check, together with the walks that resolved; the
    # before/after counts are in the commit that added this.
    # The MMU-base remainder (port series step 7).  beyond_l2_fetches_demand
    # is the walker's count of page-table lines it had to fetch from the
    # NUCA or DRAM on a demand walk, registered only now: on the TRAIL
    # substrate without an L2-TLB prefetcher every walk is a demand one, so
    # it has to accumulate while the prefetch side stays registered at zero.
    'beyond_l2_fetches': (
        ['address_translation_schemes/trail_comparison_v4/v4_noprefetch'], 1,
        require_nonzero('mmu.page_table_walks', 'mmu_walker.beyond_l2_fetches_demand',
                        registered=('mmu_walker.beyond_l2_fetches_prefetch',))),

    # perfect_l2_tlb: the L1 TLBs miss as configured, every miss is answered
    # as an L2 hit, and no walk ever happens -- so translations and the L2
    # TLB's latency (tlb_latency_1, charged only on an L1 miss answered at
    # level 1) accumulate while the walk count and the walker's beyond-L2
    # fetches stay at zero.  tlb_misses_data is zero here by construction:
    # it counts translations no TLB level answered.
    'perfect_l2_tlb': (
        ['address_translation_schemes/trail_comparison_v4/v4_perfect_l2tlb'], 1,
        lambda stats: (
            counter(stats, 'mmu.num_translations') > 0
            and counter(stats, 'mmu.tlb_latency_1') > 0
            and counter(stats, 'mmu.tlb_misses_data') == 0
            and counter(stats, 'mmu.page_table_walks') == 0
            and counter(stats, 'mmu_walker.beyond_l2_fetches_demand') == 0
            and counter(stats, 'mmu_walker.beyond_l2_fetches_prefetch') == 0,
            'translations=%d l2_hit_latency=%d tlb_misses=%d walks=%d beyond_l2_demand=%d'
            % (counter(stats, 'mmu.num_translations'),
               counter(stats, 'mmu.tlb_latency_1'),
               counter(stats, 'mmu.tlb_misses_data'),
               counter(stats, 'mmu.page_table_walks'),
               counter(stats, 'mmu_walker.beyond_l2_fetches_demand')))),

    'asp': (
        ['address_translation_schemes/baseline_asp'], 1,
        lambda stats: (
            counter(stats, 'asp_tlb.threshold_reached') > 0
            and counter(stats, 'asp_tlb.prefetch_attempts')
                == 3 * counter(stats, 'asp_tlb.threshold_reached')
            and counter(stats, 'asp_tlb.successful_prefetches') > 0,
            'threshold_reached=%d prefetch_attempts=%d (degree 3) successful=%d failed=%d'
            % (counter(stats, 'asp_tlb.threshold_reached'),
               counter(stats, 'asp_tlb.prefetch_attempts'),
               counter(stats, 'asp_tlb.successful_prefetches'),
               counter(stats, 'asp_tlb.failed_prefetches')))),

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

    # Virtualised execution: a guest MMU nested on a host MMU.  Under the
    # default MMU none of these counters exists -- there is no host MMU, no
    # guest page table and no guest OS -- so "registered" is the first thing
    # the check tells apart.  Then both stages of the nested walk have to have
    # run: the guest page table walked and faulted (its frames come from the
    # guest OS), the host page table walked and faulted for the guest-physical
    # addresses, and the host MMU translated the guest page-table accesses the
    # guest walk made -- which is what makes the walk two-dimensional rather
    # than two consecutive one-dimensional ones.
    'virt': (
        ['address_translation_schemes/virt'], 1,
        require_nonzero('mmu.num_page_table_walks',
                        'mmu.guest_page_faults',
                        'mmu.host_page_faults',
                        'mmu.total_guest_walk_latency',
                        'mmu.total_host_walk_latency',
                        'host_mmu.num_translations',
                        'host_mmu.page_table_walks',
                        'radix_4level_guest.page_table_walks',
                        'radix_4level_guest.page_faults',
                        'radix_4level_host.page_table_walks',
                        'radix_4level_host.page_faults',
                        'baseline_guest.total_allocations')),

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

    # The fixed-window stride TLB prefetcher: on every access to the prefetch
    # queue's TLB it walks the page table for the +/-length neighbours of the
    # demand page.  It registers under tlb_stride, which no other prefetcher
    # does, so the counters are absent unless prefetcher_list names it.  It
    # activates at the default geometry: the queue is consulted on every L1
    # miss, and each consultation is four candidate walks whatever the TLB
    # size -- so no stress scheme.  Both the attempts and the walks that
    # resolved are required: an attempt count alone would pass for a
    # prefetcher whose walks all come back empty.
    'stride': (
        ['address_translation_schemes/baseline_stride'], 1,
        require_nonzero('tlb_stride.prefetch_attempts',
                        'tlb_stride.successful_prefetches')),

    # The H2 TLB prefetcher: two candidate walks per access once it has seen
    # three demand pages, for the demand page plus each of the two most
    # recent strides.  It registered no statistics until now; tlb_h2.* are
    # its own, and as for stride both the attempts and the walks that
    # resolved are required.
    'h2': (
        ['address_translation_schemes/baseline_h2'], 1,
        require_nonzero('tlb_h2.prefetch_attempts',
                        'tlb_h2.successful_prefetches')),

    # The open-addressing Revelator engine: one hash gives the home frame and
    # the engine probes linearly from it, so every prediction is a set of
    # probing_depth frames.  The *_probe_N names are registered only by this
    # engine (vanilla Revelator counts per hash, THP per page size), so a run
    # on any other engine reads them as absent.  It has to predict on both
    # sides -- data and page table -- and probe 1 has to be issued as well as
    # probe 0, or the probing is not happening; and probe 0 has to hit, which
    # it can only do if the allocator put the frame at the engine's home hash
    # (revelator_simple's first hash is the same CityHash placement).  The
    # data side only runs on a walk that does not fault, hence the stress
    # scheme: see revelator_open_stress.cfg.  Probe 1 never hits here: with
    # nothing poisoned no home frame is ever taken, so no page is displaced
    # to the next frame, which is what probe 1 is for.
    'revelator_open_addressing': (
        ['address_translation_schemes/revelator_open_stress'], 1,
        require_nonzero('revelator.total_prefetches',
                        'revelator.prefetches_probe_0',
                        'revelator.prefetches_probe_1',
                        'revelator.hits_probe_0',
                        'revelator.prefetches_pt_probe_0',
                        'revelator_simple.hash1_successful_data_allocations')),

    # The Revelator THP engine on the revelator_thp allocator.  The engine
    # exits unless that allocator is selected, and the allocator feeds its
    # placement statistics back to the engine on every fault; here every
    # data fault gets a hashed 2MB region, so the feedback switches the
    # engine to 2MB-only prediction before its first invocation.  In that
    # mode it issues one prediction per hash, indexed from 0, and the slot
    # at index 0 is the one it named _4KB_0 at construction -- so the data
    # side lands in prefetches_4KB_0 / hits_4KB_0 (the _2MB_1 slot is only
    # used while predicting both sizes).  The names with a page-size infix
    # are registered only by this engine.  The data side needs the stress
    # geometry, and a harsher one than the other engines: the workload's
    # data is six 2MB pages, which fit in the TLBs every other stress scheme
    # shrinks to (revelator_thp_stress.cfg).  The hits prove the engine's
    # region hash is the allocator's; the page-table side cannot hit here
    # because a 2MB mapping has no leaf page-table frame, so only its
    # prefetches are required.
    'revelator_thp': (
        ['address_translation_schemes/revelator_thp_stress'], 1,
        require_nonzero('revelator.total_prefetches',
                        'revelator.prefetches_pt_4KB_0',
                        'revelator.prefetches_4KB_0',
                        'revelator.hits_4KB_0',
                        'revelator_thp.revelator_2mb_data_allocated',
                        'revelator_thp.hash1_successful_pt_allocations',
                        # registered only by this engine; the allocator never
                        # fell below half hash-placed frames, so it is zero
                        registered=('revelator.deactivated_invocations',))),

    # The three sniper-space allocators nothing selected, each under the
    # baseline MMU so that only the allocator differs from baseline.  Every
    # allocator registers its counters under its memory_allocator_name, so
    # the prefix is the discriminator: a run on reserve_thp has none of them.
    #
    # revelator_simple: hashed 4KB data frames and hashed leaf page-table
    # frames, nothing else.  Both hash paths have to have placed something,
    # and every data allocation has to have gone through the hash (the
    # total equals the hash count, and there was no fallback).  The
    # page-table path is only reached through the Revelator exception
    # handler, which is why the scheme selects it.
    'revelator_simple_allocator': (
        ['address_translation_schemes/baseline_revelator_simple'], 1,
        require_nonzero('revelator_simple.total_data_allocations',
                        'revelator_simple.hash1_successful_data_allocations',
                        'revelator_simple.total_hash_data_allocations',
                        'revelator_simple.total_page_table_allocations',
                        'revelator_simple.hash1_successful_pt_allocations',
                        'revelator_simple.total_hash_pt_allocations')),

    # revelator_thp: the hashed 2MB stage comes first and, with nothing
    # poisoned, serves every data fault -- so the 2MB counter is what moves
    # and the 4KB hash and THP reservation stages are never reached.  The
    # leaf page-table frames are hashed as for revelator_simple.
    'revelator_thp_allocator': (
        ['address_translation_schemes/baseline_revelator_thp'], 1,
        require_nonzero('revelator_thp.total_data_allocations',
                        'revelator_thp.revelator_2mb_data_allocated',
                        'revelator_thp.total_page_table_allocations',
                        'revelator_thp.hash1_successful_pt_allocations',
                        'revelator_thp.total_hash_pt_allocations')),

    # numa_reserve_thp: the reservation allocator per NUMA node.  A fault
    # on the local policy reserves a 2MB region on its node and promotes it
    # once the threshold is met (0.0 here, so at once), and page-table
    # frames come from the node's kernel region through this allocator's
    # own bump path, which is the only one that counts them as
    # page_table_pages_used, and the per-node counters, which the allocator
    # registers now that it allocates the per-node arrays before the policy
    # registers them: every allocation of the smoke workload lands on node 0.
    'numa_reserve_thp': (
        ['address_translation_schemes/baseline_numa_reserve_thp'], 1,
        require_nonzero('numa_reserve_thp.total_allocations',
                        'numa_reserve_thp.local_allocs',
                        'numa_reserve_thp.two_mb_reserved',
                        'numa_reserve_thp.two_mb_promoted',
                        'numa_reserve_thp.page_table_pages_used',
                        'numa_reserve_thp.node0_allocs',
                        'numa_reserve_thp.node0_2mb_reserved',
                        'numa_reserve_thp.node0_2mb_promoted')),

    # Page-size prediction at the L2 TLB.  The MMU registers the hit and
    # miss counters under every configuration and only moves them on an L2
    # hit with prediction switched on, so under every other scheme they
    # read zero.  What makes the prediction observable beyond that is the
    # latency it charges: every L2 hit costs correct_prediction_latency (4
    # cycles) or misprediction_latency (10) instead of the TLB's own 12, so
    # the level-1 latency has to come out to exactly that sum -- which it
    # cannot do if the branch is being taken without the predictor being
    # consulted, or with the flat latency still charged.  The workload is
    # all 4KB pages, so the predictor is right every time after warm-up and
    # the miss counter stays at zero; it is only required to exist.
    'page_size_prediction': (
        ['address_translation_schemes/baseline_pagesize_prediction'], 1,
        lambda stats: (
            counter(stats, 'mmu.page_size_prediction_hits') > 0
            and 'mmu.page_size_prediction_misses' in stats
            and abs(counter(stats, 'mmu.tlb_latency_1')
                    * float(stats.get('fs_to_cycles', 0) or 0)
                    - (4 * counter(stats, 'mmu.page_size_prediction_hits')
                       + 10 * counter(stats, 'mmu.page_size_prediction_misses'))) < 1,
            'hits=%d misses=%d; L2 latency %d cycles = 4*hits + 10*misses'
            % (counter(stats, 'mmu.page_size_prediction_hits'),
               counter(stats, 'mmu.page_size_prediction_misses'),
               round(counter(stats, 'mmu.tlb_latency_1')
                     * float(stats.get('fs_to_cycles', 0) or 0))))),

    # The "default" page-table-walk filter, which passes every walk through
    # untouched.  It registers nothing, and that absence is its signature:
    # the "pwc" filter every other scheme runs under builds the page walk
    # caches, which register pwc_L2..L4 (access, miss) under every one of
    # them, and announces itself on stdout.  So the check is that walks
    # happened and were charged, and that not one pwc_ counter exists and
    # the announcement never printed.  The cost is visible in the numbers
    # too -- 3.1e9 fs of walk latency for 182 walks here against 2.0e9
    # under baseline -- but a ratio against another run is not something
    # one run can assert on.
    'no_pwc': (
        ['address_translation_schemes/baseline_no_pwc'], 1,
        lambda stats: (
            counter(stats, 'radix_4level.page_table_walks') > 0
            and counter(stats, 'mmu.total_walk_latency_data') > 0
            and not any(k.startswith('pwc_') for k in stats)
            and 'Page walk caches are enabled' not in stats['__stdout__'],
            'walks=%d walk_latency=%d; no pwc_ counter registered, no PWC '
            'announcement'
            % (counter(stats, 'radix_4level.page_table_walks'),
               counter(stats, 'mmu.total_walk_latency_data')))),

    # The userspace kernel's baseline allocator: a plain buddy with no
    # reservation, promotion or per-CPU pool.  The kernel reports which
    # path every fault took, and this allocator does not say -- it has no
    # fast and slow paths to tell apart -- so every fault reports
    # alloc_path=unknown, and not one reports fast or slow, which is the
    # linux_buddy_anon signature mimicos_pcp_allocator requires.  The
    # factory's creation line pins which allocator was built, since
    # reserve_thp reports unknown as well.
    'mimicos_baseline_allocator': (
        ['address_translation_schemes/userspace_baseline'], 2,
        lambda stats: (
            'Created VirtuosoBaselineAllocator' in stats['__stdout__']
            and stats['__stdout__'].count('alloc_path=unknown') > 0
            and stats['__stdout__'].count('alloc_path=fast') == 0
            and stats['__stdout__'].count('alloc_path=slow') == 0,
            'baseline allocator built; %d faults, all alloc_path=unknown'
            % stats['__stdout__'].count('alloc_path=')),
        ('startup_mimicos', r'alloc_path=', None,
         ['mimicos/configs/baseline_32GB.ini',
          'test/sniper-in-sniper/fft.sift', '/tmp/sniper_baseline_alloc_check.txt'])),

    # one_app_sched.ini: the application comes from the [apps] section of
    # the kernel's configuration rather than from the command line, with
    # the round-robin scheduler on one kernel core -- the configuration the
    # two-application case was bisected against, and which nothing ran.
    # The kernel has to size itself from the list (one core, so no kernel
    # pthread), register the listed trace as a task at the application's
    # real thread id, run it to its exit (exactly one shutdown, since there
    # is one worker to receive it) and come out with its reports.
    'mimicos_one_app_sched': (
        ['address_translation_schemes/userspace_baseline'], 2,
        lambda stats: (
            'Scheduler] Configured: policy=rr_simple cores=1' in stats['__stdout__']
            and 'Registered app trace as Task id=0 atid=0x10000' in stats['__stdout__']
            and stats['__stdout__'].count('shutdown event received') == 1
            and 'Scheduler stats:' in stats['__stdout__']
            and 'Assertion' not in stats['__stdout__']
            and counter(stats, 'trace_thread.quanta_expired') > 0,
            'one kernel core, task at atid 0x10000, one shutdown, '
            '%d quanta expired' % counter(stats, 'trace_thread.quanta_expired')),
        ('startup_mimicos', r'Scheduler stats: csw=', None,
         ['mimicos/configs/one_app_sched.ini',
          'test/sniper-in-sniper/fft.sift', '/tmp/sniper_one_app_check.txt'])),

    # The sniper-space HugeTLBfs service: a pre-reserved pool of 2 MB pages
    # that the page-fault handler maps the application's large VMAs from.
    # The service existed with every counter structurally zero -- MimicOS
    # built it, nothing called allocateHugePage -- and this is the check that
    # it is now on the fault path end to end: a frame handed out
    # (allocations_2mb), installed as one 2 MB PTE that the walk then finds
    # (page_size_discovery_1, the radix table's level-2 hits) and translated
    # from the 2 MB DTLB (mmu_TLB_L1_2.hits, the L1 TLB whose only page size
    # is 21).  The scheme turns the allocator's own THP promotion off, so
    # those two counters have no other source: two_mb_promoted must read 0.
    #
    # The pool has two pages and the guest's region holds at least three
    # whole aligned 2 MB blocks, so exactly two frames go out, the third
    # request fails (allocation_failures, pool_exhausted_events) and that
    # fault falls back to the allocator -- both halves of the path run.
    #
    # Trace-driven, and that is not incidental: the pool backs VMAs, and
    # VMAs come only from the recorder's log (record-trace --vma), so the
    # guest is recorded first and the trace simulated.  Its checksum is
    # checked where it is produced, at recording time.
    'hugetlbfs': (
        ['address_translation_schemes/hugetlbfs'], 1,
        lambda stats: (
            counter(stats, 'hugetlbfs.allocations_2mb') == 2
            and counter(stats, 'hugetlbfs.allocation_failures') > 0
            and counter(stats, 'hugetlbfs.pool_exhausted_events') > 0
            and counter(stats, 'radix_4level.page_size_discovery_1') > 0
            and counter(stats, 'mmu_TLB_L1_2.hits') > 0
            and 'reserve_thp.two_mb_promoted' in stats
            and counter(stats, 'reserve_thp.two_mb_promoted') == 0
            and counter(stats, 'mmu.sanity_check_violations') == 0
            and 'Parsing VMAs (plain) from' in stats['__stdout__'],
            'pool of 2: allocations_2mb=%d, allocation_failures=%d, '
            'pool_exhausted_events=%d; 2MB walks=%d, 2MB DTLB hits=%d, '
            'allocator promotions=%d, sanity violations=%d'
            % (counter(stats, 'hugetlbfs.allocations_2mb'),
               counter(stats, 'hugetlbfs.allocation_failures'),
               counter(stats, 'hugetlbfs.pool_exhausted_events'),
               counter(stats, 'radix_4level.page_size_discovery_1'),
               counter(stats, 'mmu_TLB_L1_2.hits'),
               counter(stats, 'reserve_thp.two_mb_promoted'),
               counter(stats, 'mmu.sanity_check_violations'))),
        ('hugetlb_workload.c', r'\[SNIPER\] End', None, None,
         r'hugetlb_workload sum=50528256')),

    # Step 8: ASAP, the page-table-side engine of the speculative MMU.  It
    # acts only on the page-table invocation, which hands it the entries the
    # functional walk visited, and prefetches the lines of the depth-2 and
    # depth-3 entries (the PD and PT of a radix walk) into the L2 so they are
    # in flight while the first two levels resolve.  Both counters are its
    # own: invocations says the MMU called it on the walk side, hits that it
    # found entries deep enough to prefetch -- an engine fed only the
    # data-side invocation would register both and move neither.
    'asap': (
        ['address_translation_schemes/asap'], 1,
        require_nonzero('asap.asap_invocations', 'asap.asap_hits')),

    # Step 9: the oracle data-side engine, the upper bound of address
    # speculation.  The MMU invokes it after every walk that did not fault,
    # with the physical address the walk resolved, and it prefetches that
    # line at the time the walk started.  Its two counters are its own; the
    # L2's prefetch count is what shows the line was actually issued to the
    # cache rather than counted and dropped.  It needs the stress geometry
    # for the reason revelator_thp does: under reserve_thp every data fault
    # gets a 2MB page, and at the default geometry no walk is fault-free, so
    # the data side never runs (spec_oracle_stress.cfg).
    'oracle_spec': (
        ['address_translation_schemes/spec_oracle_stress'], 1,
        require_nonzero('oracle.invocations', 'oracle.prefetches', 'L2.prefetches')),

    # Step 10: SpecTLB, the reserved-region data-side engine.  A TLB miss
    # looks the address up in a small TLB of reserved 2MB regions and, on a
    # hit, prefetches the data line at the region's base plus the offset
    # within 2MB; a miss installs the region after the walk, if the
    # reserve_thp allocator reports it reserved.  The whole chain has to
    # run: lookups (spec_tlb.accesses, registered by the TLB it builds),
    # regions installed, hits that issued a prefetch, and predictions that
    # were right -- under reserve_thp every 4KB page of a reserved region
    # sits at its offset, so a hit's line is the translation's own.  Needs
    # the stress geometry for the reason oracle_spec does.
    'spectlb': (
        ['address_translation_schemes/spectlb_stress'], 1,
        require_nonzero('spec_tlb.accesses', 'spec_tlb.hits',
                        'spectlb.invocations', 'spectlb.allocations',
                        'spectlb.hits', 'spectlb.correct_predictions',
                        'L2.prefetches')),

    # Step 13: the hashed page table with chaining, under the baseline MMU
    # on the sniper-space fault path end to end.  The page table is
    # selected by the OS (page_table_type), the MMU walks whatever it is
    # given, and the fault handler maps into it; so the walk counters have
    # to move (registered under the table's name, hash_table_chaining, one
    # per page size), faults have to have been resolved through
    # updatePageTableFrames (page_faults, and the application reaching its
    # checksum, which run_sniper requires of every case), and the walks
    # have to have been charged (mmu.total_walk_latency_data).  The
    # conflicts counter is registered but a few hundred pages in 65536
    # slots never chain, so it only has to exist.
    'pagetable_ht': (
        ['address_translation_schemes/ht_baseline_alloc'], 1,
        require_nonzero('hash_table_chaining.page_table_walks_12',
                        'hash_table_chaining.page_table_walks_21',
                        'hash_table_chaining.accesses_12',
                        'hash_table_chaining.page_faults',
                        'mmu.page_table_walks',
                        'mmu.total_walk_latency_data',
                        registered=('hash_table_chaining.conflicts_12',
                                    'hash_table_chaining.resizes_12'))),

    # Step 14: Hash, Don't Cache, the linear-probing hashed page table, on
    # the same path as pagetable_ht: walks per page size under the table's
    # name (pt_hash_hdc), faults resolved through updatePageTableFrames,
    # walks charged, checksum reached.  Collisions and resizes only have
    # to exist, for the same reason.
    'pagetable_hdc': (
        ['address_translation_schemes/hdc_baseline_alloc'], 1,
        require_nonzero('pt_hash_hdc.page_table_walks_12',
                        'pt_hash_hdc.page_table_walks_21',
                        'pt_hash_hdc.accesses_12',
                        'pt_hash_hdc.page_faults',
                        'mmu.page_table_walks',
                        'mmu.total_walk_latency_data',
                        registered=('pt_hash_hdc.collisions_12',
                                    'pt_hash_hdc.resizes_12'))),

    # Step 15: the elastic cuckoo page table with its Cuckoo Walk Cache.
    # The table's walks (ptws_total, hit_at_level0: the 4KB table resolved
    # them, num_accesses0: the ways probed), faults resolved through
    # updatePageTableFrames, and the checksum, as for the other hashed
    # tables; plus the walk filter, which this scheme selects with
    # ptw_filter_type = cwc: the CWC was consulted on every walk
    # (mmu_cwc.cwc_accesses) and, the workload living in a handful of 1GB
    # rows, hit (mmu_cwc.cwc_hits).  Evictions and rehashes only have to
    # exist: a few hundred pages in 262144 slots never cuckoo.
    'pagetable_ech': (
        ['address_translation_schemes/ech_baseline_alloc'], 1,
        require_nonzero('pt_elastic_cuckoo.ptws_total',
                        'pt_elastic_cuckoo.hit_at_level0',
                        'pt_elastic_cuckoo.num_accesses0',
                        'pt_elastic_cuckoo.page_faults',
                        'mmu_cwc.cwc_accesses',
                        'mmu_cwc.cwc_hits',
                        'mmu.page_table_walks',
                        'mmu.total_walk_latency_data',
                        registered=('pt_elastic_cuckoo.evictions',
                                    'pt_elastic_cuckoo.rehashes'))),

    # Step 11: Direct Memory Translation.  A TLB miss costs one memory
    # access, the line of the leaf entry, instead of the radix walk's four:
    # the functional walk is the radix table's, the charge is the one line.
    # The proof is the walker's accounting: every charged walk read exactly
    # one line (dmt_leaf_accesses, the MMU's own counter, equals the
    # walker's memory_accesses), the demand faults were charged nothing (so
    # the charged walks are the walks less the faults), no walk fell back to
    # the radix charge, and the page walk caches -- built, since the config
    # enables them -- were never consulted.  The checksum says the
    # translations were right.
    'dmt': (
        ['address_translation_schemes/dmt'], 1,
        lambda stats: (
            counter(stats, 'mmu.page_table_walks') > 0
            and counter(stats, 'mmu.page_faults') > 0
            and counter(stats, 'mmu.dmt_leaf_accesses')
                == counter(stats, 'mmu.page_table_walks') - counter(stats, 'mmu.page_faults')
            and counter(stats, 'mmu_walker.memory_accesses') == counter(stats, 'mmu.dmt_leaf_accesses')
            and 'mmu.dmt_fallback_walks' in stats
            and counter(stats, 'mmu.dmt_fallback_walks') == 0
            and 'pwc_L2.access' in stats
            and counter(stats, 'pwc_L2.access') == 0
            and counter(stats, 'pwc_L3.access') == 0
            and counter(stats, 'pwc_L4.access') == 0
            and counter(stats, 'mmu.total_table_walk_latency') > 0,
            'walks=%d faults=%d leaf_accesses=%d walker_accesses=%d fallback=%d '
            'pwc_accesses=%d walk_latency=%d'
            % (counter(stats, 'mmu.page_table_walks'),
               counter(stats, 'mmu.page_faults'),
               counter(stats, 'mmu.dmt_leaf_accesses'),
               counter(stats, 'mmu_walker.memory_accesses'),
               counter(stats, 'mmu.dmt_fallback_walks'),
               counter(stats, 'pwc_L2.access') + counter(stats, 'pwc_L3.access') + counter(stats, 'pwc_L4.access'),
               counter(stats, 'mmu.total_table_walk_latency')))),

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


def record_trace(app, name, expect):
    """Record a guest with the SDE recorder's VMA logging, once per build.

    `record-trace --vma -o <dir>/trace` writes trace.sift and trace.vma, the
    latter the address ranges of /proc/self/maps, which is the only VMA
    information a simulation ever sees (MimicOS loads it next to the trace).
    The guest's output is checked here, where it is produced: a stored trace
    replays no syscalls, so the simulation prints nothing of the guest's.
    Kept beside the goldens for the same reason the binaries are, and
    re-recorded when the binary is newer.  Returns the .sift path.
    """
    tracedir = os.path.join(GOLDEN_DIR, name + '.trace')
    sift = os.path.join(tracedir, 'trace.sift')
    vma = os.path.join(tracedir, 'trace.vma')
    if (os.path.exists(sift) and os.path.exists(vma)
            and os.path.getmtime(sift) >= os.path.getmtime(app)):
        return sift, None
    shutil.rmtree(tracedir, ignore_errors=True)
    os.makedirs(tracedir)
    cmd = []
    if have_setarch():
        cmd += ['setarch', '-R']
    cmd += [os.path.join(SNIPER_ROOT, 'record-trace'), '--vma',
            '-o', os.path.join(tracedir, 'trace'), '--', app]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, cwd=SNIPER_ROOT, env=run_env(),
                       start_new_session=True)
    if p.returncode != 0:
        return None, 'record-trace exit %d' % p.returncode
    if not expect.search(p.stdout):
        return None, 'recorded guest produced no output'
    if not (os.path.exists(sift) and os.path.exists(vma)):
        return None, 'record-trace wrote no trace.sift/trace.vma'
    return sift, None


def run_sniper(names, ncores, outdir, workload, timeout, expect=WORKLOAD_OUTPUT,
               extra_env=None, app_args=None, traces=None):
    """One simulation, with ASLR pinned so the statistics are reproducible.

    With `traces`, a trace-driven run (--traces=) of those .sift files instead
    of a live guest; `workload` is then ignored.
    """
    shutil.rmtree(outdir, ignore_errors=True)
    cmd = []
    if have_setarch():
        cmd += ['setarch', '-R']
    cmd += [os.path.join(SNIPER_ROOT, 'run-sniper'), '-n', str(ncores)]
    # a name starting with '-' is passed to run-sniper as it is (e.g. '-slive:50000')
    cmd += [n if n.startswith('-') else '-c' + n for n in names]
    cmd += ['-d', outdir]
    if traces:
        cmd += ['--traces=' + ','.join(traces)]
    else:
        cmd += ['--', workload]
        cmd += list(app_args or [])
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
    # Every line of the run stream is stamped and tagged (tools/run_sniper.py,
    # "The run stream"); a line that is not means a producer bypassed the
    # tee, or the tee split or lost one.  Same regex as the lnav format.
    bad = [l for l in p.stdout.splitlines() if l.strip() and not STAMPED.match(l)]
    if bad:
        return None, 'unstamped output line: %r' % bad[0][:60]
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
            traces = None
            if len(entry) == 4:
                cfgs, ncores, check, guest = entry
                # (source, output_pattern) or (source, output_pattern, env)
                # or (source, output_pattern, env, args); a fifth element is
                # the guest's own output pattern and makes the run
                # trace-driven: the guest is recorded with the VMA log first
                # (record_trace) and the simulation replays the trace, so
                # output_pattern is then what the simulation prints.
                src, pattern = guest[0], guest[1]
                extra_env = guest[2] if len(guest) > 2 else None
                app_args = guest[3] if len(guest) > 3 else None
                record_pattern = guest[4] if len(guest) > 4 else None
                if src.endswith('.c'):
                    app = build_app(src, os.path.splitext(src)[0])
                elif src == 'startup_mimicos':
                    # The kernel lands in whichever build directory is in use;
                    # run_tests.py already knows how to find it, so use its
                    # resolver rather than keeping a second rule here.
                    sys.path.insert(0, HERE)
                    from run_tests import resolve_mimicos_binary
                    app = resolve_mimicos_binary(SNIPER_ROOT)
                    if not os.path.isfile(app):
                        print('SKIP (startup_mimicos not built)')
                        continue
                else:
                    app = os.path.join(SNIPER_ROOT, src)
                    if not os.path.exists(app):
                        print('SKIP (%s not built)' % src)
                        continue
                expect = re.compile(pattern)
            else:
                cfgs, ncores, check = entry
                app, expect = workload, WORKLOAD_OUTPUT
                extra_env = None
                app_args = None
                record_pattern = None
            print('%-22s ' % fname, end='')
            sys.stdout.flush()
            started = time.time()
            outdir = os.path.join(workdir, 'feat-' + fname)
            err = None
            if record_pattern:
                sift, err = record_trace(app, fname, re.compile(record_pattern))
                traces = [sift]
            if not err:
                out, err = run_sniper(cfgs, ncores, outdir, app, args.timeout,
                                      expect, extra_env, app_args, traces)
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
            feat_stats['__outdir__'] = outdir
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
