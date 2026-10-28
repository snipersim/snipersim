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
    # The sweep's workload is an x86-64 program, and the simulator refuses a
    # trace of another architecture than the configured core's.  These cores
    # are simulated with AArch64 traces in testing/test_qemu.py (aarch64-*).
    'cortex-a53': 'ARM core, x86-64 workload: refused (the trace ISA check)',
    'cortex-a72': 'ARM core, x86-64 workload: refused (the trace ISA check)',
    # Likewise the RISC-V cores, simulated with RISC-V traces in test_qemu.py
    # (riscv-*; docs/RISCV_AI_ACCELERATOR_STACK.md).  Before the check, the
    # x86 workload "passed" in fast-forward, which decodes nothing, and
    # asserted in the RISC-V decoder in detailed mode.
    'riscv': 'RISC-V core, x86-64 workload: refused (the trace ISA check)',
    'riscv-mediumboom': 'RISC-V core, x86-64 workload: refused (the trace ISA check)',
    'kingscross': 'mesh topology fixed at 16:4, so it needs 64 cores',
    'sampling': 'sampling is only supported with the Pin frontend',
}

# EXPECT_FAIL entries that are fragments, tested layered on the base the way
# they are used: run alone, sampling.cfg failed on the cache hierarchy it does
# not carry, which says nothing about the reason listed for it.
EXPECT_FAIL_LAYERED = {'sampling'}

# The error each expected failure must fail with, matched against the
# simulator's *ERROR* text (or the harness's own message).  An expected
# failure used to accept any failure: a crash, or an error of another kind,
# passed as "known".  Required for every entry of EXPECT_FAIL,
# EXPECT_FAIL_DETAILED and FRAGMENT_EXPECT_FAIL.
EXPECTED_ERROR = {
    'beckton': r'Caches of non-power of 2 size',
    'cortex-a53': r'The trace is x86-64 code, but this configuration',
    'cortex-a72': r'The trace is x86-64 code, but this configuration',
    'riscv': r'The trace is x86-64 code, but this configuration',
    'riscv-mediumboom': r'The trace is x86-64 code, but this configuration',
    'core_configs/cortex_a53': r'The trace is x86-64 code, but this configuration',
    'core_configs/cortex_a55': r'The trace is x86-64 code, but this configuration',
    'core_configs/cortex_a720': r'The trace is x86-64 code, but this configuration',
    'core_configs/cortex_x4': r'The trace is x86-64 code, but this configuration',
    'core_configs/oryon_xelite': r'The trace is x86-64 code, but this configuration',
    'kingscross': r'Invalid mesh size 16:4',
    'sampling': r'Sampling is only supported in Pin mode',
    'physical_memory_allocators/eager_paging': r'no VMAs for application',
    'address_translation_schemes/rmm': r'no VMAs for application',
    'pagetable_configs/3_level_radix_sv39': r'does not fit the 39-bit virtual addre',
    'dram_configs/m3d': r'missing config value perf_model/dram/ddr/address_mapping',
    'dram_configs/readwrite_2ctrl_153gbps': r"Num Memory Controllers\(2\)|DRAM controllers may only be at 'master' node",
    'mmu_configs/mmu_spec': r'missing config value perf_model/mmu/spec/type',
    'mmu_configs/mmu_virt': r'The virt MMU needs a host MMU',
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
    # Step 19: eager paging lays a VMA out whole on its first fault, and
    # VMAs come only from a trace's VMA log; this suite's live workload has
    # none, so the first fault stops the run with a message saying so
    # (fast-forward never faults).  test_virtuoso's eager_paging case runs
    # it trace-driven.
    'physical_memory_allocators/eager_paging': 'eager paging needs the trace\'s VMAs (none in a live run)',
    # RISC-V's Sv39 page table over an x86-64 workload: the guest's stack sits
    # near 2^47, above the 39-bit space, and the walk refuses it (the riscv
    # configurations include this table; the sweep layers every fragment on
    # gainestown).  Fast-forward walks no page table and runs.
    'pagetable_configs/3_level_radix_sv39': 'RISC-V Sv39 (39-bit) page table; an x86-64 guest lives above 2^39 and is refused',
    # Step 20: the RMM scheme pages eagerly; test_virtuoso's rmm case runs
    # it trace-driven.
    'address_translation_schemes/rmm': 'eager paging needs the trace\'s VMAs (none in a live run)',
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
# fragments under spec_engine_configs/ used not to be enumerated, because an
# engine only runs under the spec MMU with its allocator; they are now, each
# with that MMU and allocator in front of it (FRAGMENT_EXTRA_BASE), so the
# engine-only fragments Virtuoso ships (the lean selector and the standalone
# parameter block) get run as well.
FRAGMENT_DIRS = ('address_translation_schemes', 'common_configs', 'core_configs',
                 'cxl_configs', 'dram_configs', 'mmu_configs', 'pagetable_configs',
                 'physical_memory_allocators', 'prefetcher_configs',
                 'spec_engine_configs',
                 # The multicore schemes: each includes a scheme and sets its
                 # own core count, which FRAGMENT_CORES repeats so that the
                 # run has the cores the config was written for.
                 'address_translation_schemes/multicore',
                 # The TRAIL comparison matrix: one substrate (ip_stride +
                 # streamer data prefetchers, CHiRP L2 TLB), one L2-TLB
                 # prefetcher per file.
                 'address_translation_schemes/trail_comparison_v4',
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
    # Overrides on an MMU config: the CHiRP L2-TLB replacement policy, and
    # the TRAIL prefetch-queue block (self-contained for the prefetcher but
    # not for the TLBs it attaches to).  base.cfg has the mmu_base keys too,
    # but the override is meant for the config it documents.
    'mmu_configs/l2tlb_chirp': ['mmu_configs/mmu_base'],
    'mmu_configs/trail_aggressive': ['mmu_configs/mmu_base'],
    # A speculative engine runs under the spec MMU, over the allocator whose
    # placement it predicts; the Revelator engines soft-disable themselves
    # over any other allocator, and the NUMA one reads the NUMA topology.
    'spec_engine_configs/spec_engine_revelator':
        ['mmu_configs/mmu_spec', 'physical_memory_allocators/revelator_alloc'],
    'spec_engine_configs/spec_engine_revelator_open':
        ['mmu_configs/mmu_spec', 'physical_memory_allocators/revelator_simple_alloc'],
    'spec_engine_configs/spec_engine_revelator_thp':
        ['mmu_configs/mmu_spec', 'physical_memory_allocators/revelator_thp_alloc'],
    'spec_engine_configs/spec_engine_revelator_simple':
        ['mmu_configs/mmu_spec', 'physical_memory_allocators/revelator_simple_alloc'],
    'spec_engine_configs/spec_engine_numa_revelator':
        ['dram_configs/ddr4_2400', 'numa', 'mmu_configs/mmu_spec',
         'physical_memory_allocators/numa_revelator_alloc'],
    # The lean selector sets only the engine type and the parameter block
    # only the engine's knobs: the pair is Virtuoso's way of running the
    # Revelator engine standalone over reserve_thp (where it soft-disables).
    'spec_engine_configs/spec_engine_revelator_lean':
        ['mmu_configs/mmu_spec', 'physical_memory_allocators/reserve_thp',
         'spec_engine_configs/revelator_engine_params'],
    'spec_engine_configs/revelator_engine_params':
        ['mmu_configs/mmu_spec', 'physical_memory_allocators/reserve_thp',
         'spec_engine_configs/spec_engine_revelator_lean'],
    # Step 17: the SpOT engine predicts the SpOT allocator's placement and
    # reads its table geometry from the allocator's block.
    'spec_engine_configs/spec_engine_spot':
        ['mmu_configs/mmu_spec', 'physical_memory_allocators/spot'],
    # Step 23: the Utopia MMU walks the RestSegs of the Utopia allocator;
    # over any other allocator it has none and only walks the page table.
    'mmu_configs/mmu_utopia': ['physical_memory_allocators/utopia'],
    # Step 24: the coalesce MMU over the coalescing allocator
    'mmu_configs/mmu_utopia_coalesce': ['physical_memory_allocators/utopia_coalesce'],
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
    # One big core and one little one: every per-core key is a two-entry
    # array (prefetcher[] = "ip_stride", "ip_stride"; rob_timer/
    # issue_memops_at_issue[] = ...), and a third core has no entry to read.
    'core_configs/biglittle': (1, 2),
    # The core count each multicore scheme sets for itself.
    'address_translation_schemes/multicore/parametric_baseline_2c': (2,),
    'address_translation_schemes/multicore/parametric_baseline_4c': (4,),
    'address_translation_schemes/multicore/parametric_baseline_8c': (8,),
    'address_translation_schemes/multicore/parametric_baseline_16c': (16,),
    'address_translation_schemes/multicore/parametric_asp_4c': (4,),
}

# Known to fail in detailed mode at one core count only, with the reason;
# listed rather than skipped so that one starting to work is reported.
# (rob_smt2 at two logical cores used to be here: the SMT ROB timer drained a
# departed thread's memory uops against a core with no thread.  Fixed; the
# case must pass, at both core counts.)
FRAGMENT_EXPECT_FAIL_DETAILED_CORES = {
}

# Known to fail intermittently in detailed mode at one core count: the run is
# reported as passed either way, with the outcome of this run in the note, so
# the suite stays quiet while the defect is on record and loud about nothing.
FRAGMENT_INTERMITTENT_DETAILED_CORES = {
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
    # Step 26 of the Virtuoso port series: each predictor's own counter
    # (branch_predictor.* is registered by every predictor alike).
    'core_configs/gshare_branch_predictor': ('gshare.updates', 'branch_predictor.num-correct'),
    'core_configs/perceptron_branch_predictor': ('perceptron.trainings', 'branch_predictor.num-correct'),
    'core_configs/hashed_perceptron_branch_predictor': ('hashed_perceptron.trainings', 'branch_predictor.num-correct'),
    'core_configs/tage_branch_predictor': ('tage.tagged_provided', 'tage.allocations', 'branch_predictor.num-correct'),
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
    # Step 8: ASAP registers under its own name and only the speculative MMU
    # invokes it; both schemes layer that MMU with the engine.
    'address_translation_schemes/asap': ('asap.asap_invocations', 'asap.asap_hits'),
    'address_translation_schemes/parametric_asap': ('asap.asap_invocations', 'asap.asap_hits'),
    # Step 9: the oracle engine registers under its own name.  Only the
    # stress scheme invokes it: at the default geometry every walk of this
    # workload faults (see spec_oracle_stress.cfg), and the engine stays idle.
    'address_translation_schemes/spec_oracle_stress': ('oracle.invocations', 'oracle.prefetches'),
    # Step 10: SpecTLB's own TLB registers under spec_tlb and the engine
    # under spectlb; as for the oracle only the stress scheme exercises
    # them (see spectlb_stress.cfg).
    'address_translation_schemes/spectlb_stress': ('spec_tlb.accesses', 'spectlb.hits'),
    # Step 13: the hashed page table with chaining registers its walks
    # under its own name, one counter per page size.
    'address_translation_schemes/ht_baseline_alloc': ('hash_table_chaining.page_table_walks_12', 'hash_table_chaining.page_faults'),
    'address_translation_schemes/reservethp_ht': ('hash_table_chaining.page_table_walks_12', 'hash_table_chaining.page_faults'),
    # Step 14: Hash, Don't Cache registers under pt_hash_hdc.
    'address_translation_schemes/hdc_baseline_alloc': ('pt_hash_hdc.page_table_walks_12', 'pt_hash_hdc.page_faults'),
    'address_translation_schemes/reservethp_hdc': ('pt_hash_hdc.page_table_walks_12', 'pt_hash_hdc.page_faults'),
    # Step 15: the elastic cuckoo table registers under pt_elastic_cuckoo
    # and its Cuckoo Walk Cache under mmu_cwc.
    'address_translation_schemes/ech_baseline_alloc': ('pt_elastic_cuckoo.ptws_total', 'pt_elastic_cuckoo.page_faults', 'mmu_cwc.cwc_accesses'),
    'address_translation_schemes/reservethp_ech': ('pt_elastic_cuckoo.ptws_total', 'pt_elastic_cuckoo.page_faults', 'mmu_cwc.cwc_accesses'),
    # The Virtuoso configs ported in one piece (VIRTUOSO_PORT_SERIES.md
    # step 3), each on the counter its selection alone registers or drives:
    #   chirp     CacheSetInfoCHiRP registers chirp-* under the TLB's cache
    #             name and nothing else does; this workload hardly misses
    #             the L2 TLB (a handful of alive-correct predictions at one
    #             core, none at two), so the counters are only required to
    #             exist (FRAGMENT_REQUIRE_REGISTERED).
    #   trail     tlb_temporal_pte.* are the TRAIL prefetcher's; at the
    #             default geometry it sees the demand accesses but learns too
    #             little to predict reliably (a couple of predictions with
    #             one address layout, none with another: this suite does not
    #             pin ASLR), so only those are required.  test_virtuoso's
    #             trail case is where the chain is proved, on the stress
    #             geometry under setarch -R.
    #   dp/atp/recency/temporal_pte
    #             the TLB prefetcher each MMU config selects, on its queries.
    #   perfect_translation
    #             translates (num_translations) without ever walking: the
    #             walk count is required to be ZERO (FRAGMENT_REQUIRE_ZERO).
    #   no_translation
    #             general/translation_enabled = false: the MMU is built but
    #             never asked, so num_translations is required to be zero.
    #   pwc_stressed
    #             a one-entry PWC misses at every level; victima's leaf probe
    #             then has to answer (leaf_hits).
    #   reserve_thp / reserve_asap
    #             the reserve_thp allocator registers under its type, so the
    #             asap-named instance counts as reserve_thp too.
    #   spec engines
    #             total_prefetches is the engine's; the allocator counter
    #             says it was the one the engine predicts for.  The lean
    #             selector and the parameter block soft-disable over
    #             reserve_thp, so they only have to run.
    #   ddr       the detailed DDR model's row-buffer accounting.
    #   ghb_l1    the L1-D controller's prefetch count, zero without one.
    'mmu_configs/trail_aggressive': ('tlb_temporal_pte.demand_accesses',),
    'mmu_configs/mmu_distance': ('dp_tlb.queries', 'dp_tlb.predictions_issued'),
    'address_translation_schemes/baseline_distance': ('dp_tlb.queries', 'dp_tlb.predictions_issued'),
    'address_translation_schemes/parametric_atp': ('tlb_atp.queries',),
    'address_translation_schemes/parametric_recency': ('recency_tlb.queries',),
    'address_translation_schemes/parametric_distance': ('dp_tlb.queries', 'dp_tlb.predictions_issued'),
    'address_translation_schemes/parametric_temporal_pte': ('tlb_temporal_pte.demand_accesses',),
    'address_translation_schemes/perfect_translation': ('mmu.num_translations',),
    'address_translation_schemes/perfect_translation_xor': ('mmu.num_translations',),
    'address_translation_schemes/perfect_translation_1c_tlb':
        ('mmu.num_translations', 'mmu.total_tlb_latency'),
    'address_translation_schemes/baseline_pwc_stressed':
        ('pwc_L2.miss', 'pwc_L3.miss', 'pwc_L4.miss'),
    'address_translation_schemes/victima_pwc_stressed':
        ('pwc_L2.miss', 'mmu.victima_leaf_probes', 'mmu.victima_leaf_hits'),
    'address_translation_schemes/reservethp': ('reserve_thp.total_allocations',),
    'address_translation_schemes/reservethp_4channel': ('reserve_thp.total_allocations',),
    'physical_memory_allocators/reserve_thp_4k': ('reserve_thp.four_kb_allocated',),
    'physical_memory_allocators/reserve_asap': ('reserve_thp.total_allocations',),
    'spec_engine_configs/spec_engine_revelator':
        ('revelator.total_prefetches', 'revelator_allocator.hash1_tries'),
    'spec_engine_configs/spec_engine_revelator_open':
        ('revelator.total_prefetches', 'revelator_simple.total_data_allocations'),
    'spec_engine_configs/spec_engine_revelator_thp':
        ('revelator.total_prefetches', 'revelator_thp.total_data_allocations'),
    'spec_engine_configs/spec_engine_revelator_simple':
        ('revelator.total_prefetches', 'revelator_simple.total_data_allocations'),
    'spec_engine_configs/spec_engine_numa_revelator':
        ('numa_revelator.total_prefetches', 'numa_revelator_allocator.hash1_tries'),
    'dram_configs/ddr4_2666_6ch': ('ddr.page-hits', 'ddr.page-miss'),
    'dram_configs/ddr5_4800_4channel': ('ddr.page-hits', 'ddr.page-miss'),
    'prefetcher_configs/ghb_l1': ('L1-D.prefetches',),
    # The ip_stride and streamer data prefetchers (port series step 4),
    # on the counter each registers for the addresses it generated and the
    # controller's count of the prefetches it issued for them.  The core
    # configs that select ip_stride at the L1-D keep L2 at none, except
    # wimpy; biglittle's per-core array puts the streamer on core 1 only,
    # which a one-core run never builds, so only ip_stride is required.
    'core_configs/meteor_lake_pcore': ('ip_stride.gen_prefetch', 'L1-D.prefetches'),
    'core_configs/beefy': ('ip_stride.gen_prefetch', 'L1-D.prefetches'),
    'core_configs/wimpy': ('ip_stride.gen_prefetch', 'streamer.gen_prefetch',
                           'L1-D.prefetches', 'L2.prefetches'),
    'core_configs/revelator_core': ('ip_stride.gen_prefetch', 'L1-D.prefetches'),
    'core_configs/cascade_lake_xeon': ('ip_stride.gen_prefetch', 'L1-D.prefetches'),
    'core_configs/biglittle': ('ip_stride.gen_prefetch', 'L1-D.prefetches'),
    'prefetcher_configs/ip_stride_l1': ('ip_stride.gen_prefetch', 'L1-D.prefetches'),
    'prefetcher_configs/streamer_l2': ('streamer.gen_prefetch', 'L2.prefetches'),
    'prefetcher_configs/aggressive_multilevel':
        ('ip_stride.gen_prefetch', 'streamer.gen_prefetch', 'L1-D.prefetches', 'L2.prefetches'),
    'prefetcher_configs/conservative':
        ('ip_stride.gen_prefetch', 'streamer.gen_prefetch', 'L1-D.prefetches', 'L2.prefetches'),
    'prefetcher_configs/datapf_ipstride_notrans_streamer':
        ('ip_stride.gen_prefetch', 'streamer.gen_prefetch', 'L1-D.prefetches', 'L2.prefetches'),
    'prefetcher_configs/meteor_lake_prefetch':
        ('ip_stride.gen_prefetch', 'streamer.gen_prefetch', 'L1-D.prefetches', 'L2.prefetches'),
    # The SPP, SMS, Berti and IPCP data prefetchers (step 5), each on its
    # own issue count and the controller's.
    'prefetcher_configs/spp_l2': ('spp.prefetches_issued', 'L2.prefetches'),
    'prefetcher_configs/sms_l1': ('sms.prefetches_issued', 'L1-D.prefetches'),
    'prefetcher_configs/berti_l1': ('berti.prefetches_issued', 'L1-D.prefetches'),
    'prefetcher_configs/ipcp_l1': ('ipcp.prefetches_issued', 'L1-D.prefetches'),
    # The ASP TLB prefetcher with lookahead and degree (step 6): the prefetch
    # queue is on, ASP is trained (threshold_reached), and it attempts and
    # resolves prefetches.  The 1:3 ratio of threshold_reached to attempts
    # (degree = 3) is checked by test_virtuoso's asp case.
    'mmu_configs/mmu_base_prefetch':
        ('asp_tlb.queries', 'asp_tlb.threshold_reached', 'asp_tlb.prefetch_attempts',
         'asp_tlb.successful_prefetches'),
    'address_translation_schemes/baseline_asp':
        ('asp_tlb.threshold_reached', 'asp_tlb.prefetch_attempts', 'asp_tlb.successful_prefetches'),
    'address_translation_schemes/parametric_asp':
        ('asp_tlb.threshold_reached', 'asp_tlb.prefetch_attempts', 'asp_tlb.successful_prefetches'),
    'address_translation_schemes/parametric_asp_datapf':
        ('asp_tlb.prefetch_attempts', 'ip_stride.gen_prefetch', 'streamer.gen_prefetch'),
    'address_translation_schemes/parametric_baseline_datapf':
        ('ip_stride.gen_prefetch', 'streamer.gen_prefetch', 'L1-D.prefetches', 'L2.prefetches'),
    'address_translation_schemes/multicore/parametric_asp_4c': ('asp_tlb.prefetch_attempts',),
    # The TRAIL comparison matrix (step 7), each file on the L2-TLB
    # prefetcher(s) it selects, on the substrate's data prefetchers, and on
    # the walker's new count of lines fetched from beyond the L2.
    #   perfect_l2tlb  translates without ever walking: walks and the
    #                  beyond-L2 fetches are required to be zero.
    'address_translation_schemes/trail_comparison_v4/v4_noprefetch':
        ('mmu_walker.beyond_l2_fetches_demand', 'ip_stride.gen_prefetch', 'streamer.gen_prefetch'),
    'address_translation_schemes/trail_comparison_v4/v4_perfect_l2tlb':
        ('mmu.num_translations', 'mmu.tlb_hits_data', 'ip_stride.gen_prefetch'),
    'address_translation_schemes/trail_comparison_v4/v4_asp':
        ('asp_tlb.prefetch_attempts', 'mmu_walker.beyond_l2_fetches_demand'),
    'address_translation_schemes/trail_comparison_v4/v4_asp_atp':
        ('asp_tlb.prefetch_attempts', 'tlb_atp.queries'),
    'address_translation_schemes/trail_comparison_v4/v4_asp_berti':
        ('asp_tlb.prefetch_attempts', 'tlb_berti.strides_learned'),
    'address_translation_schemes/trail_comparison_v4/v4_asp_dp':
        ('asp_tlb.prefetch_attempts', 'dp_tlb.queries'),
    'address_translation_schemes/trail_comparison_v4/v4_asp_recency':
        ('asp_tlb.prefetch_attempts', 'recency_tlb.queries'),
    'address_translation_schemes/trail_comparison_v4/v4_asp_stride':
        ('asp_tlb.prefetch_attempts', 'tlb_stride.prefetch_attempts'),
    'address_translation_schemes/trail_comparison_v4/v4_dp': ('dp_tlb.queries', 'dp_tlb.predictions_issued'),
    'address_translation_schemes/trail_comparison_v4/v4_recency': ('recency_tlb.queries',),
    'address_translation_schemes/trail_comparison_v4/v4_trail': ('tlb_temporal_pte.demand_accesses',),
    'address_translation_schemes/trail_comparison_v4/v4_trail_sidecar': ('tlb_temporal_pte.demand_accesses',),
    # Step 11: Direct Memory Translation registers the leaf accesses it
    # charged under the MMU's name; the page walk caches it never consults
    # are required to read zero (below).
    'mmu_configs/mmu_dmt': ('mmu.page_table_walks', 'mmu.dmt_leaf_accesses'),
    'address_translation_schemes/dmt': ('mmu.page_table_walks', 'mmu.dmt_leaf_accesses'),
    # Step 12: the POM-TLB's own TLB objects count their accesses, and the
    # MMU the lookups' memory latency.
    'mmu_configs/mmu_pomtlb': ('pom_tlb_0.accesses', 'mmu.software_tlb_lookups', 'mmu.total_software_tlb_latency'),
    'address_translation_schemes/reservethp_pomtlb': ('pom_tlb_0.accesses', 'mmu.software_tlb_lookups', 'mmu.total_software_tlb_latency'),
    'address_translation_schemes/pomtlb_stress': ('pom_tlb_0.accesses', 'mmu.software_tlb_lookups', 'mmu.software_tlb_hits'),
    # Step 16: the SpOT allocator on a live workload has no VMAs to place,
    # so every fault is a buddy page (no_vma_allocations) and nothing is
    # placed (vmas_placed zero, below); test_virtuoso's trace-driven case
    # is where the placement is proved.
    'physical_memory_allocators/spot': ('spot_allocator.no_vma_allocations', 'mmu.page_faults'),
    # Step 17: the SpOT engine under the spec MMU over the SpOT allocator,
    # which on the live workload places nothing (no_vma_allocations).  The
    # engine's lookup follows a fault-free walk, and at the default geometry
    # every walk of this workload faults (see spec_oracle_stress.cfg), so
    # its counters are only required to exist (FRAGMENT_REQUIRE_REGISTERED)
    # and learn nothing (allocations zero); test_virtuoso's trace-driven
    # spot case is where the engine is proved.
    'address_translation_schemes/spot': ('spot_allocator.no_vma_allocations',),
    'address_translation_schemes/parametric_spot': ('spot_allocator.no_vma_allocations',),
    'spec_engine_configs/spec_engine_spot': ('spot_allocator.no_vma_allocations',),
    # Step 20: the range MMU looks every TLB miss up in its RLB; with no
    # eager-paging ranges (the default allocator) every lookup misses and
    # nothing is resolved by range (zero, below).
    'mmu_configs/mmu_rmm': ('rlb.accesses', 'rlb.misses', 'mmu.page_table_walks'),
    # Step 22: the Utopia allocator registers under its name; restseg_allocations
    # is the placement only it makes (under the baseline MMU of the layered
    # run the fault handler maps the RestSeg pages in the page table as well).
    'physical_memory_allocators/utopia': ('utopia.total_allocations', 'utopia.restseg_allocations'),
    'physical_memory_allocators/utopia_4k': ('utopia.total_allocations', 'utopia.restseg_allocations'),
    # Step 23: the Utopia MMU translates RestSeg pages without the page
    # table: the RestSeg walk after a fault (data_in_restseg) and the UTLB
    # (utlb.accesses) are its own.
    'mmu_configs/mmu_utopia': ('mmu.data_in_restseg', 'utlb.accesses'),
    'address_translation_schemes/utopia': ('mmu.data_in_restseg', 'utlb.accesses'),
    'address_translation_schemes/utopia_smoke': ('mmu.data_in_restseg', 'utlb.accesses'),
    'address_translation_schemes/utopia_mplru': ('mmu.data_in_restseg', 'utlb.accesses'),
    'address_translation_schemes/utopia_mplru_bandit': ('mmu.data_in_restseg', 'utlb.accesses'),
    # The stress scheme's one-entry UTLB hits only when the same page misses
    # the TLBs twice running, which depends on the address layout this
    # suite does not pin (3-4 hits with one layout, none with another):
    # test_virtuoso's utopia_mmu case is where data_in_utlb is proved.
    'address_translation_schemes/utopia_stress': ('mmu.data_in_restseg', 'mmu.data_in_restseg_no_fault'),
    'address_translation_schemes/utopia_fpatar': ('mmu.data_in_restseg', 'fpacache.access', 'tarcache.access'),
    # Step 24: the coalescing allocator's promotions and the MMU's 2MB walk.
    'physical_memory_allocators/utopia_coalesce': ('utopia_coalesce.coalescing_promotions', 'utopia_coalesce.restseg_allocations'),
    'mmu_configs/mmu_utopia_coalesce': ('mmu.radix_2mb_lookups', 'mmu.data_in_restseg'),
    'address_translation_schemes/utopia_coalesce': ('utopia_coalesce.coalescing_promotions', 'mmu.radix_2mb_lookups', 'mmu.data_in_restseg'),
    # Step 25: the ASAP allocator registers under its type name.
    'physical_memory_allocators/asap': ('asap.four_kb_allocated', 'asap.total_allocations'),
}

# Counters that must merely exist after the detailed run (test_virtuoso's
# `registered`): a variant's own counter that this workload legitimately
# leaves at zero, where being registered is still what tells the variant
# from its absence.
FRAGMENT_REQUIRE_REGISTERED = {
    'mmu_configs/l2tlb_chirp':
        ('mmu_TLB_L2_1_cache.chirp-alive-correct', 'mmu_TLB_L2_1_cache.chirp-dead-correct'),
    'address_translation_schemes/baseline_chirp':
        ('mmu_TLB_L2_1_cache.chirp-alive-correct', 'mmu_TLB_L2_1_cache.chirp-dead-correct'),
    # Step 17: the SpOT engine's own counters, idle on the live workload.
    'address_translation_schemes/spot': ('spot.cache_accesses', 'spot.predictions_made'),
    'address_translation_schemes/parametric_spot': ('spot.cache_accesses', 'spot.predictions_made'),
    'spec_engine_configs/spec_engine_spot': ('spot.cache_accesses', 'spot.predictions_made'),
}

# Counters that must exist and read ZERO after the detailed run: the
# evidence of a feature that removes something.  Checked in detailed mode,
# like the non-zero ones, and always together with a non-zero counter from
# the same run (above) so that an empty run cannot pass by having nothing.
FRAGMENT_REQUIRE_ZERO = {
    'address_translation_schemes/perfect_translation': ('mmu.page_table_walks', 'mmu.total_translation_latency'),
    'address_translation_schemes/perfect_translation_xor': ('mmu.page_table_walks', 'mmu.total_translation_latency'),
    'address_translation_schemes/perfect_translation_1c_tlb': ('mmu.page_table_walks',),
    'address_translation_schemes/no_translation': ('mmu.num_translations',),
    'address_translation_schemes/no_translation_xor': ('mmu.num_translations',),
    'address_translation_schemes/trail_comparison_v4/v4_perfect_l2tlb':
        ('mmu.page_table_walks', 'mmu_walker.beyond_l2_fetches_demand'),
    # Step 11: DMT reads the leaf line directly; the page walk caches are
    # built (the config enables them) and never consulted.
    'mmu_configs/mmu_dmt': ('pwc_L2.access', 'mmu.dmt_fallback_walks'),
    'address_translation_schemes/dmt': ('pwc_L2.access', 'mmu.dmt_fallback_walks'),
    # Step 16: no VMAs on the live workload, so nothing is placed.
    'physical_memory_allocators/spot': ('spot_allocator.vmas_placed', 'spot_allocator.offset_allocations'),
    # Step 17: nothing placed, so nothing learned.
    'address_translation_schemes/spot': ('spot_allocator.vmas_placed', 'spot.allocations'),
    'address_translation_schemes/parametric_spot': ('spot_allocator.vmas_placed', 'spot.allocations'),
    'spec_engine_configs/spec_engine_spot': ('spot_allocator.vmas_placed', 'spot.allocations'),
    # Step 20: no ranges, so nothing resolved by range.
    'mmu_configs/mmu_rmm': ('mmu.requests_resolved_by_rlb', 'mmu.requests_resolved_by_range_table'),
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
    # The ARM cores of the core-aggressiveness sweep, built on the cortex-a53
    # and cortex-a72 core models: an x86-64 workload is refused like the
    # cortex configurations'.  testing/test_qemu.py (aarch64-core-configs)
    # simulates them with AArch64 traces.
    'core_configs/cortex_a53': 'ARM core, x86-64 workload: refused (the trace ISA check)',
    'core_configs/cortex_a55': 'ARM core, x86-64 workload: refused (the trace ISA check)',
    'core_configs/cortex_a720': 'ARM core, x86-64 workload: refused (the trace ISA check)',
    'core_configs/cortex_x4': 'ARM core, x86-64 workload: refused (the trace ISA check)',
    'core_configs/oryon_xelite': 'ARM core, x86-64 workload: refused (the trace ISA check)',
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


def build_dir():
    """The build run-sniper will use: $SNIPER_BUILD_DIR, else build/default."""
    sys.path.insert(0, os.path.join(SNIPER_ROOT, 'tools'))
    import run_sniper
    return run_sniper.resolve_build_dir(SNIPER_ROOT)


def build_features():
    """What the build was configured with, as a set.

    The build directory's CMakeCache.txt records every SNIPER_ENABLE_*
    option.  Read from there rather than guessed from the source tree,
    because the question is what this binary can do -- and from the build
    run-sniper will use, not from lib/sniper: with $SNIPER_BUILD_DIR set to
    a libtorch build, the nn branch predictor was skipped as "build has no
    torch" while run-sniper would have run it.
    """
    cache = os.path.join(build_dir(), 'CMakeCache.txt')
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


def check_nonzero(outdir, names, zeros=(), registered=()):
    """Every named counter must exist and be non-zero (test_virtuoso's
    require_nonzero, on a results directory); every one in `zeros` must
    exist and be zero; every one in `registered` must exist."""
    try:
        stats = read_stats(outdir)
    except Exception as e:
        return False, 'cannot read statistics: %s' % e
    missing = [n for n in names + tuple(zeros) + tuple(registered) if n not in stats]
    if missing:
        return False, 'counter not registered: %s' % ', '.join(missing)
    zero = [n for n in names if counter(stats, n) == 0]
    if zero:
        return False, 'counter stayed zero: %s' % ', '.join(zero)
    nonzero = [n for n in zeros if counter(stats, n) != 0]
    if nonzero:
        return False, 'counter not zero: %s' % ', '.join(
            '%s=%d' % (n, counter(stats, n)) for n in nonzero)
    return True, ', '.join('%s=%d' % (n, counter(stats, n))
                           for n in names + tuple(zeros) + tuple(registered))



def load_factor():
    """How much slower than on an idle host a case runs now: the run queue
    per CPU, when there is more than one runnable thread for each.  The
    limits below are for a healthy run on a quiet host; under another
    account's jobs (load 80 to 150 on 80 CPUs) fourteen detailed cases ran
    out of their two minutes, the same on every build, and passed with
    longer limits.  A deadlock still ends at a bounded multiple."""
    try:
        per_cpu = os.getloadavg()[0] / (os.cpu_count() or 1)
    except OSError:
        return 1.0
    return min(max(per_cpu, 1.0), 8.0)


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
               expect_output=None, counters=(), app='workload', zeros=(),
               registered=()):
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
        if (counters or zeros or registered) and mode == 'detailed':
            ok, detail = check_nonzero(outdir, counters, zeros, registered)
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
    ap.add_argument('--no-load-scaling', action='store_true',
                    help='do not lengthen the limits by the host\'s run queue per CPU (see load_factor)')
    ap.add_argument('--keep', action='store_true',
                    help='keep the work directory (printed at the end) instead of removing it')
    add_failfast(ap)
    ap.add_argument('--mode', action='append', choices=MODES,
                    help='run only this mode (repeatable; default: both)')
    args = ap.parse_args()

    if not os.path.exists(os.path.join(build_dir(), 'lib', 'sniper')):
        print('error: %s/lib/sniper not built; run make first' % build_dir(), file=sys.stderr)
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
            'zeros': FRAGMENT_REQUIRE_ZERO.get(fragment, ()),
            'registered': FRAGMENT_REQUIRE_REGISTERED.get(fragment, ()),
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
            names = [layer_base, n] if n in EXPECT_FAIL_LAYERED else [n]
            out.append(case(' + '.join(names), names, False, EXPECT_FAIL[n]))
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
                    scale = load_factor() if not args.no_load_scaling else 1.0
                    limit = int(limit * scale)
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
                                            c['counters'], c['app'], c['zeros'],
                                            c['registered'])
                    took = time.time() - started
                    if scale > 1.0 and not ok and 'timed out' in detail:
                        detail += ' (the limit scaled %.1fx for the host\'s load)' % scale
                    if flaky_note and want_ok:
                        log.record('%s [%s]' % (full, mode), took, 'PASS')
                        print('PASS (known intermittent, %s this time: %s) [%ds]'
                              % ('ran' if ok else 'failed', flaky_note, round(took)))
                        continue
                    if not ok and not want_ok:
                        # An expected failure must be the expected one.
                        key = next((n for n in reversed(names) if n in EXPECTED_ERROR), None)
                        if key is None:
                            ok, want_ok, detail = False, True, 'expected to fail, but no EXPECTED_ERROR recorded for it (failed with: %s)' % detail
                        elif not re.search(EXPECTED_ERROR[key], detail):
                            ok, want_ok, detail = False, True, 'failed for another reason than the known one (%s): %s' % (why, detail)
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
        elif args.keep or failures:      # a failed check keeps the directory whatever --keep says: an intermittent failure leaves its evidence
            print('work directory kept at %s' % workdir)
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
