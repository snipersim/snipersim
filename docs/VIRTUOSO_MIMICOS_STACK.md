# The Virtuoso / MimicOS simulator stack

Reference for the simulator as it stands on branch `dev-mimicos-v2`: the
Sniper core it grew from, the build and dependency system, the frontends,
the memory-hierarchy and address-translation work of Virtuoso v2 (TRAIL,
Revelator, Victima, the hardware fault path), the two halves of MimicOS
(the host model inside the simulator and the userspace kernel that runs
under it), the run stream and live feed, Pac-Sim, and every regression
suite.  The RISC-V side, the accelerator models and the AI workloads are
in the companion `RISCV_AI_ACCELERATOR_STACK.md` (this directory).

Every command in this document was run, in the order written, on the
commit named in the verification record at the end.  Commands sit in
`bash` blocks; the first block of section 4 sets the environment the
others assume.  Blocks whose first line starts with `# verify:` are the
ones the verification pass executes verbatim (`tools/doc-verify.py
DOC.md > run.sh` writes that pass as one script, `--long` the long
blocks); the two blocks that need
resources not present on every host (a ChampSim trace, `lnav`) are
marked as such.

---

## 1. What is here

| piece | where | what it does |
|---|---|---|
| The simulator | `common/`, `lib/sniper` | Sniper: interval and out-of-order (`rob`) core models, private and shared caches with a MESI directory protocol, DRAM models, a Python scripting interface, McPAT power |
| Build and dependencies | `Makefile`, `CMakeLists.txt`, `cmake/`, `deps/sniper-deps`, `deps/manifest.json` | A CMake build behind a `make` forwarder; every external dependency declared once with a pinned version and digest, fetched into a shared content-addressed cache |
| Frontends | `record-trace`, `sift/recorder/` (the SIFT recorder as an SDE tool), `frontend/dr-frontend/` (DynamoRIO, x86-64 and ARM64), `frontend/qemu-plugin/` (QEMU), `sift/champsim/` | Every frontend produces SIFT, the trace format the simulator reads live or from a file |
| Memory models | `common/performance_model/dram_perf_model_*.cc`, `tiered_dram_cntlr.cc` | Constant, read/write, normal, detailed DDR with row buffers and address mapping, CXL.mem Type-3, a tiered controller over any mix |
| Address translation | `common/core/memory_subsystem/parametric_dram_directory_msi/` (`mmu_designs/`, `translation_components/`, `spec_engine_designs/`, `page_table_filters/`) | The MMU framework: radix page walks, page-walk caches, a two-level TLB hierarchy with eight prefetchers (TRAIL among them), speculative translation (Revelator), Victima, hardware page faults |
| MimicOS, host side | `common/system/memory_management/` | The operating-system model inside the simulator: page tables, physical memory allocators (buddy, reservation THP, NUMA, Revelator), kswapd/khugepaged/kcompactd, hugetlbfs, swap, exception handlers |
| MimicOS, userspace kernel | `mimicos/` (`build/default/bin/startup_mimicos`) | A kernel that runs as a guest program under simulation, drives applications from their traces, takes their page faults, schedules them, and talks to the simulator only through magic instructions |
| Multicore and NUMA | `common/misc/ticket_lock.h`, `common/system/barrier_sync_server.cc`, `include/memory_management/numa/` | A fair ticket lock, barrier core parking, NUMA-aware allocation, the `none` clock-skew scheme |
| Output | `tools/run_sniper.py`, `tools/lnav/`, `scripts/live.py`, `tools/sniper-live` | One stamped, tagged run stream from every process; an lnav format for it; a live feed in simulated time and a terminal dashboard |
| Pac-Sim | `sift/recorder/pacsim.{h,cc}`, `tools/pacsim/`, `tools/pacsim.py`, `tools/dumpstats.py`, `run-sniper --pacsim` | Sampled simulation that chooses, while the application runs, what to simulate in detail and what to keep warm, and estimates the whole run's time; the recorder side was restored on this branch (section 3.7) |
| Tests | `testing/test_{scripting,configs,qemu,pacsim,apps,virtuoso}.py`, `testing/run_tests.py`, `make regress` | Six suites behind one `make regress`; an invariance-and-activation suite for every Virtuoso feature; an MMU experiment runner |

---

## 2. How it got here

The branch (`dev-mimicos-v2-update`) is Sniper 7.4 plus the work below, in order of landing:

1. **Sniper 8.1 and its tooling.**  The SDE-based recorder as the default frontend (Pin and PinPlay selectable), DynamoRIO support, a first Pac-Sim, the branch predictor merge.
2. **The DRAM series.**  A refactored `DramPerfModel` interface carrying the access type through the hierarchy, a pluggable address-mapping framework, a detailed DDR model with row-buffer simulation, a CXL.mem Type-3 model, a tiered heterogeneous controller, and the DDR and CXL configuration files.
3. **The MMU series.**  The MMU framework (radix walks, TLB hierarchy, prefetchers, filters, page-walk caches), the MimicOS host framework, and their integration with the cache controller and memory manager.
4. **MimicOS as a userspace program.**  The standalone kernel, dual-trace SIFT accessors, dynamic instruction ownership and page-fault support in the core, VMA tracking in the recorder, magic handlers and the kernel-mode bypass.
5. **ChampSim traces, multicore and NUMA.**  The ChampSim reader and frontend, the ticket lock and barrier parking, NUMA infrastructure and the reservation-based THP allocator, multicore and NUMA configurations.
6. **The build.**  Python 3 scripting, the recursive make replaced by CMake with presets, dependencies declared in a manifest and fetched into a shared cache, the in-Pin simulator retired, the SDE recorder built with one PinCRT SIFT, ccache, out-of-tree builds with one owner per tree.
7. **Frontends and correctness.**  The QEMU TCG-plugin frontend (x86 user and full system) and the probe that settled its design; the QSim frontend retired; a double free in every SIFT reader, a set-lock downgrade in the cache controller, self-modifying code made to simulate, MPI supplied and made to run, several recorder and syscall fixes.
8. **Testing.**  Every configuration run at one, two and four cores in both modes; a scripting-hooks suite; a shipped-configuration sweep; checks on what the `test/` workloads produce, not only that they ran; Pac-Sim restored as a real entry point with a validation that sampling is faster and agrees; timing reports; `--failfast`.
9. **Virtuoso v2**, staged by blast radius: inert additions (O), config-gated features (F: speculative translation and Revelator, Victima, hardware faults, the Linux daemons, the userspace substrate), invasive changes one number-moving commit at a time (I: non-power-of-two caches and TLBs, sub-line prefetch credit, the prefetch hooks, TRAIL and Berti, Agile, Recency and Distance), and the frontend surgery (K: the reserved magic commands, the per-thread ROI barrier, fork children, per-core event queues, the reader pool, shutdown, quanta, fast-fault replay, the in-tree kernel).
   What the staged port left out, by decision: Midgard (no live lines upstream), `address_space.{h,cc}` (unused), the `fast_detailed` configurations, the duplicate `.cc` allocators that would have collided with the templates.  The Revelator exception handler, planned as step F4 and missed, is in the tree since the release cleanup, as are the `virt` (nested) MMU and the NUMA Revelator engine and allocator; what upstream Virtuoso still has beyond this tree is listed in the release notes.
10. **Output and oversubscription.**  One stamped stream from every process, the lnav format, the live feed and dashboard, and more threads than cores no longer deadlocking the SDE frontend.
11. **RISC-V and the accelerators** (the companion document).

---

## 3. Components

### 3.1 Building

`make` forwards to CMake in three phases that never fuse: `make deps`
(the fetcher), `make configure`, `make build`.  Feature flags go to both
the fetcher and CMake so the two cannot disagree:

| make variable | default | effect |
|---|---|---|
| `FRONTEND` | `sde` | `sde`, `pin`, `pinplay` or `none`: which recorder kit is fetched and built |
| `TORCH`, `MCPAT` | `ON` | the libtorch branch predictor and McPAT power model |
| `RISCV` | `ON` | the RISC-V core model and decoder |
| `QEMU` | `OFF` | QEMU 11.1.1 with TCG plugins (x86 user and system) and the frontend plugin |
| `RISCV_IME` | `OFF` | the patched riscv64 QEMU and the Sail model |
| `ARM`, `DYNAMORIO` | `OFF` | the ARM decoder (Capstone) and the DynamoRIO frontend |
| `BUILD_DIR` | `build/default` | where the build lands; `lib/sniper` in the tree is a symlink into it |
| `CMAKE_ARGS`, `DYNAMORIO_DIR`, `BUILD_TYPE` | | passed through |

`cmake --list-presets` shows the presets (`x86-sde-release`,
`x86-sde-debug`, `x86-pin-release`, `x86-modern-abi`, `riscv-release`,
`arm64-frontend`); a preset builds into `build/<preset>` and `run-sniper
--build-dir=` or `SNIPER_BUILD_DIR` picks it.  `deps/sniper-deps status`
lists what the current feature set needs and what is present;
`deps/manifest.json` pins every dependency (SDE 10.13.1, Pin 3.31, XED,
McPAT 1.0, libtorch 2.5.0, Capstone, DynamoRIO, QEMU 11.1.1, sail-riscv
0.14.1, MPICH 4.2.3) with a digest, and the cache is shared across
checkouts under `~/.cache/sniper/deps`.

### 3.2 Running

`run-sniper` takes the core count (`-n`), configurations (`-c`, layered
left to right; `section/key=value` also works), single overrides (`-g
--section/key=value`), the output directory (`-d`), scripts (`-s
name[:args]`), and one of: a command line after `--`, `--traces=` for
stored SIFT or ChampSim traces, `--pinballs=`, `--pid=` or
`--trace-manual`.  Other options: `--roi` (only the region of interest is
simulated in detail), `--fast-forward`, `--cache-only`,
`--no-cache-warming`, `--pacsim`, `--smc` (self-modifying code), `--live`,
`--mpi`, `--power`, `--viz`, `--profile`, `--gdb`, `--build-dir=`,
`--frontend=`, `--sim-end=`.  An explicit `-n` beats a configuration's
`total_cores`.  Output lands in `<dir>/simulation/` (`sim.cfg`,
`sim.out`, `sim.stats`, `sim.stdout`, `sim.info`), `logs/` and
`telemetry/`; `tools/dumpstats.py` and `tools/cpistack.py` read it.

Frontends, all producing SIFT:

| frontend | selected by | notes |
|---|---|---|
| SDE (default) or Pin | `FRONTEND=` at build; `record-trace` | live: the recorder streams to the simulator through FIFOs with flow control; the recorder follows forks and threads |
| PinPlay pinballs | `--pinballs=<basename>` | recorded with SDE's `-log`, replayed with address translation (`test/pinplay`) |
| DynamoRIO | `DYNAMORIO=ON`; `record-trace --frontend=dr` | x86 and the ARM64 preset |
| QEMU, x86 user mode | `QEMU=ON`; `qemu-x86_64 -plugin lib/libsniper-qemu.so,output=...` | single-threaded guests with flow control; multi-threaded guests get one stream per thread |
| QEMU, full system | `qemu-system-x86_64 -plugin ...,time,ns=1` | Sniper owns guest time; simulating such a trace waits on the decoder following the CPU mode |
| ChampSim | `--traces=x.champsim.gz` | detected by extension or by a missing SIFT magic |
| Stored SIFT | `--traces=a.sift,b.sift` | one application per trace, or one per thread with `--response-traces` |

### 3.3 Memory models

`[perf_model/dram] type` selects `constant`, `readwrite`, `normal`,
`ddr` (banks, bank groups, ranks, channels, row buffer, refresh; address
mapping `open_page`, `open_page_split_column`, `closed_page` or `xor`
under `[perf_model/dram/ddr]`) or `cxl` (CXL.mem Type-3: link, protocol
and device latencies, FIFOs, a backend DRAM, optional tiering policies
`address_range`, `first_touch`, `hot_cold`, `interleave`).  With
`[perf_model/dram/tiering] enabled` the tiered controller composes any
number of tiers, each its own model, optionally split per NUMA node
(`[perf_model/dram/numa]`).  Configuration files:
`config/dram_configs/` (`constant_latency`, `ddr4_2400`,
`ddr4_2400_4channel`, `ddr5_4800`, `ddr5_6400`, `readwrite_*`) and
`config/cxl_configs/` (`cxl`, `cxl-asic`, `cxl-fpga`, `cxl-tiered`,
`cxl-3tier`).  The detailed model reports page hits, empties and misses,
row closes, column utilisation histograms, and the per-stage queueing
delays (bank pending, rank, bank group, bus).

### 3.4 Address translation

Everything is selected by configuration, layered as scheme = base system
+ MMU + page table + allocator (`config/address_translation_schemes/*.cfg`
are the shipped combinations; `config/common_configs/base_system_{sniperspace,userspace}.cfg`
choose whether MimicOS runs inside the simulator or as the userspace
kernel).

**MMU types** (`[perf_model/mmu] type`): `default` (a four-level radix
walk through the page-walk caches with `page_table_walkers` MSHRs and
page-size prediction), `spec` (speculative translation; the engine under
`[perf_model/mmu/spec] type` is `revelator`, `revelator_open_addressing`,
`revelator_thp` or `numa_revelator`), `victima` (leaf page-table entries cached in the
L2 as `VICTIMA_TLB_BLOCK` lines, probed on an L2-TLB miss), `hw_fault`
(page faults taken in hardware).  Statistics under `mmu.*`
(`num_translations`, `page_faults`, `page_table_walks`, `tlb_hits_data`,
`tlb_misses_data`, `total_translation_latency`, the Victima probe
counters, `page_faults_hw_handled`), `mmu_walker.*` (where the walks
hit), `pwc_L*.access/miss`, `radix_4level.*`.

**TLB hierarchy** (`[perf_model/mmu/tlb_subsystem]`,
`tlb_level_<L>/tlb<N>/{type,size,assoc,page_size_list,access_latency}`):
any number of levels and TLBs per level, non-power-of-two sizes, prefetch
queues with any of eight prefetchers by name in `prefetcher_list`:

| name | prefetcher | knobs under `pq<N>/` |
|---|---|---|
| `temporal_pte` | TRAIL: temporal PTE-offset prediction with per-PTE payloads (256-bit `PayloadWord`), confidence policies, chaining, an optional virtualised PC table | `temporal_pte_prefetcher` |
| `berti` | Berti: best-request-time strides | `berti_prefetcher` |
| `atp` | Agile: H2, MASP and stride children behind a feedback director | `atp_prefetcher` |
| `recency` | recency lists built from TLB victims | `recency_prefetcher` |
| `dp` | distance prefetching | `dp_prefetcher` |
| `asp`, `stride`, `h2` | arbitrary stride, stride, H2 | `asp_prefetcher`, `stride_prefetcher` |

Every prefetcher reports its queries, predictions, prefetch attempts and
successes under its own category (`tlb_temporal_pte`, `tlb_berti`,
`tlb_atp`, `recency_tlb`, `dp_tlb`, `asp_tlb`, `tlb_stride`), and the
cache credits MMU prefetches per 8-byte sub-line
(`L2.hits-prefetch-subline`).  Four of the five prefetchers added by
Virtuoso v2 stay silent at the shipped TLB geometry (TRAIL never sees a
delta repeat, Agile's enable counter never crosses its threshold,
Recency's list needs evictions), which is why the `*_stress` schemes
shrink the TLBs.

**Revelator** (`address_translation_schemes/revelator.cfg`) needs its
allocator (`physical_memory_allocators/revelator_alloc.cfg`, hash-based
placement so the engine can predict a frame from its virtual page) and
its exception handler; its counters are `revelator.*` (`hits_<i>`,
`prefetches_<i>`, `total_prefetches`) and `revelator_allocator.hash*`.

**NUMA Revelator** (`address_translation_schemes/numa_revelator.cfg`) is
the same hash placement scoped to a NUMA node: `numa.cfg` gives the
tiered DRAM controller its nodes (`[perf_model/dram/numa]`, a
`remote_latency_ns` penalty, `dram.numa_node<n>_{local,remote}_accesses`),
`physical_memory_allocators/numa_revelator_alloc.cfg` the allocator that
hashes within the node the placement policy picks (`local`, `bind`,
`interleave`, `preferred`; counters `numa_revelator_allocator.*`:
`hash*`, `node<n>_allocs`, `node<n>_spills`, `local_allocs`), and
`spec_engine_configs/spec_engine_numa_revelator.cfg` the engine that
predicts a frame per candidate node, choosing the nodes from PTW-trained
saturating counters with a radix NUMA hint table as the fallback
(counters `numa_revelator.*`: `prefetches_<i>`, `prefetches_pt_<i>`,
`node<n>_predictions`, `train_updates`, `hint_lookups`, `hint_hits`,
`counter_confident`).  The engine's data-side prediction only runs on a
walk that does not fault, which at the default TLB geometry a smoke
workload never produces; `numa_revelator_stress.cfg` shrinks the TLBs so
it does, and is what the `numa_revelator` feature check runs.

### 3.5 MimicOS

**Host side** (`common/system/memory_management/mimicos.cc`,
constructed before the core manager because the MMUs consult it as they
are built): `[perf_model/mimicos_host]` names the page table (`radix`,
`config/pagetable_configs/4_level_radix.cfg`), the physical memory
allocator (`baseline`, `reserve_thp`, `numa_reserve_thp`, `revelator`,
`revelator_thp`, `revelator_simple`, `numa_revelator`;
`config/physical_memory_allocators/`),
`page_fault_latency`, swap, hugetlbfs, and the Linux daemon models
`kswapd`, `khugepaged` and `kcompactd` (each with `_enabled`, a scan
interval and thresholds, default off; counters `kswapd.*`,
`khugepaged.*`, `kcompactd.*`).  `[general] exception_handler_type`
picks how a page fault is served (`default`, `revelator`), and
`enable_userspace_mimicos = true` routes faults to the userspace kernel
instead.

**The userspace kernel** (`mimicos/`, target `startup_mimicos`, built
non-PIE because its fault path is compiled at `-O0` for calibration):
a guest program run as the application command line,

```
run-sniper -n <cores> -caddress_translation_schemes/userspace_baseline -d <dir> -- \
   build/default/bin/startup_mimicos <config.ini> <app.sift> <report.txt>
```

It reads an INI (`mimicos/configs/`): `[allocator]` (`linux_buddy_anon`,
`reserve_thp`, `baseline`), `[pmem_alloc]` (memory size, fragmentation
target and type, kernel size), `[apps]` (one trace per application; the
kernel takes one core per application and spawns a worker thread per
extra core), `[scheduler]` (`fifo`, `rr`, `cfs`, `vm_aware`; timeslice,
yield after fault), `[hugetlbfs]`, `[swap]`, `[kernel_arena]`,
`[fault_calibration]`, `[fault_replay]`, `[fast_fault]`.  Shipped:
`linux_buddy_anon_32GB.ini`, `linux_buddy_anon_sched_rr.ini`,
`reservethp_32GB.ini`, `one_app_sched.ini`, `two_app_smp.ini`.

It talks to the simulator through magic instructions (`include/sim_api.h`):
`SimStartProcess`, `SimReceiveMessage` (parks the caller on an empty
per-core event queue), `SimContextSwitchTo` (switches the core to an
application's trace reader from the reader pool), `SimGetInjectedAppId`,
`SimFastFault` and `SimSetFastFaultCharge` (a replayed page fault costed
in one bucket, `performance_model.cpiFastFault`), the per-thread ROI
barrier (`SimThreadRoiStart/End`, `SimWaitForRoi`, `SimRoiExpect`).
Events reach the kernel per core: `new_thread`, `page_fault`, `syscall`,
`thread_exit`, `quantum_expired` (every `scheduler/quantum_instructions`
instructions; counter `trace_thread.quanta_expired`) and a synthesized
shutdown.  The run stream tags the kernel's lines `[kernel]`; its exit
reports go to the report file.  Sniper's own scheduler stays
`scheduler/type = static` in the userspace configuration, since the
kernel picks a core's event queue from the posting thread's core.

### 3.6 Output

Every process of a run (simulator, recorder, application, kernel) writes
into one stream: each line stamped with its arrival time and tagged
`[sniper]`, `[app0]`, `[kernel]` or `[rankN]`, kept whole (one line, one
write).  It ends in `<dir>/simulation/sim.stdout`; the Virtuoso suite
refuses any unstamped line.  `tools/lnav/formats/sniper/sniper_run.json`
is an lnav format for it (`lnav -I tools/lnav <dir>/simulation/sim.stdout`;
`:filter-in kernel`, SQL over `sniper_run`).

`-s live[:interval_ns[:path]]` writes the live feed, one JSON record per
line in simulated time: per core instructions retired and time in each
mode, the CPI stack, the thread table, and every thread event,
instrumentation change, marker and kernel event between samples.
`tools/sniper-live` draws it in the terminal (`--run -- run-sniper ...`
to start and watch a run, a path or a named pipe to tail one, `--once`
for a single frame without a TTY, `--log` for the run stream in the
bottom quarter); `run-sniper --live` does both in one flag.

### 3.7 Pac-Sim

`run-sniper --pacsim` (or `--dynamic-sampling`) runs a workload sampled:
the recorder decides, while the program runs, which stretches of it the
simulator models in detail and which it only keeps warm, and the run ends
with an estimate of the whole program's simulated time (`[PacSim]
Estimated sim time ...`; `pacsim.py -d <dir>` recomputes it from the
output).  This is Pac-Sim (Liu, Sabu, Chaudhari, Kang, Carlson, TACO
2024): live sampling for multi-threaded programs, with no profiling run.

**What runs where.**  The simulator side (the magic commands
`SIM_CMD_GET_SIM_TIME`, `GET_L2_COUNT`, `GET_L2_MISS_COUNT`,
`INSTRUMENT_MODE`; `run-sniper`'s plumbing; `tools/pacsim/`, which turns
the recorder's region file into the estimate; `testing/test_pacsim.py`)
was in the tree.  The recorder side was not (removed with the upstream
recorder on 2026-03-22, commit `f472416`); it is now
`sift/recorder/pacsim.{h,cc}`, a self-contained sampler written against
the current recorder rather than a port of the 7,400-line original, with
the original's method:

- **Regions.**  Every basic block of every thread is counted (an
  instruction count and a 16-dimension basic-block vector per thread, in
  every mode).  A region ends at a loop back-edge once the threads have
  run `sniper:pacsim-region-size` thousand instructions between them
  (default 50,000K), or at a synchronisation point -- `pthread_barrier_wait`,
  `GOMP_barrier`, `GOMP_parallel`, the outlined `_omp_fn` regions of GCC's
  OpenMP, `MPI_Barrier` -- once every active thread has reached it.  The
  last arrival cuts, so that the threads' phases line up inside a region:
  cutting at the first arrival puts the other threads' tails into the next
  region, and a tail run by one thread looks nothing like the phase it
  belongs to.  A region shorter than `sniper:pacsim-min-region-size`
  (default 2,000K) joins the next.  Any thread may cut; the cut takes a
  mutex without ever blocking on it, because the cutting thread waits for
  the simulator's answer and that answer needs every other thread to keep
  delivering instructions.
- **Clusters and prediction.**  A finished region is clustered by the
  L1 distance between block mixes: its vectors summed over the threads
  that ran at least 1% of its longest thread, unit-normalised
  (`sniper:pacsim-cluster-threshold`, default 0.2), and only with regions
  that had the same number of such threads.  The sum is what makes a
  thread that lags a little -- it started late, it was descheduled --
  harmless: its own vector shows the previous phase's tail, the region's
  mix shows a small admixture.  The thread count is what keeps a thread
  catching up with the others apart from all of them running: the
  simulator's barrier lets a lagging core run alone until it has caught
  up, and the region cut while it does contains the same code as a full
  region but costs a fraction of the global time.  Each cut point
  keeps a predictor: the clusters seen after it, and the cluster history's
  longest repeating suffix.  The region about to start is fast-forwarded
  when its predicted cluster has been measured at least
  `sniper:pacsim-min-measurements` times (default 2: the first instance of
  a phase carries its warm-up, the second is what the later ones cost) and
  simulated in detail otherwise.
- **Modes.**  Detail is the recorder's detailed mode plus
  `SIM_CMD_INSTRUMENT_MODE` detailed on the simulator; fast-forward is the
  recorder's memory-only mode plus cache warm-up on the simulator, so the
  caches, TLBs and page tables see every access and the next measured region
  does not start cold (`sniper:pacsim-warmup 0` counts instructions only,
  faster and colder).  The pre-ROI region keeps the usual cache warm-up.
  `run-sniper --pacsim` also sets the fast-forward model's barrier quantum
  to the detailed model's 100 ns (its default is 100 us): with the long
  quantum one thread could run a whole region ahead of the others on its
  own, and the sampler, which cuts regions in the order the recorder sees
  them, took that solo stretch for a region of its own.
- **Measurement and the file.**  A measured region's time and L2 traffic
  are the deltas of the simulator's time and L2 counters, asked through
  the cutting thread's own channel.  The time is the furthest any core has
  got (`SIM_CMD_GET_SIM_TIME`, the figure a run reports as its elapsed
  time), so that the deltas over a run add up to the run; the barrier's
  global time, which is the slowest core's, double-counts the span a core
  that started late catches up over while the barrier holds the others.
  An unmeasured region is
  extrapolated at the end of the run from the latest measurement of its
  cluster at its own cut point, else at any cut point, else the nearest
  measured region by vector, else the average.  At ROI end the sampler
  measures the last region and writes `<output dir>/pacsim_regions.json`
  (`feature_stack[]` with `insnums`, `bbv`, `fs`, `l2count`, `l2misscount`,
  `clusterid`, `pc`, `recordtype`, `feature_level` 1 = measured);
  `tools/pacsim/pacsim_dumpjson.py` sums it, re-extrapolating what it can
  from earlier measurements and keeping the recorder's figure otherwise.

`record-trace` still checks the built recorder for the `sniper:pacsim`
knob and refuses `--pacsim` without it, so a build without the sampler
cannot silently run unsampled.  `--trace-args=-sniper:pacsim-<knob> <value>`
passes any of the knobs above.

**What was found on the way, and fixed.**  Three things in the recorder
and simulator that any workload could hit, not only a sampled one:
an aborted emulated futex wait (the application is exiting) returned
`ETIMEDOUT`, which glibc treats as fatal for a wait it gave no timeout
("The futex facility returned an unexpected error code") -- it returns
`EINTR` now; after the simulator has ended the application at ROI end,
a sibling thread that kept emulating its system calls into a closed
channel got the failure back as a return value -- emulation stops for
every thread once the ROI's `exit_group` and End have gone out
(`emulation_ended`); and the SIFT writer read whatever a closed channel
returned into an uninitialised record, so a garbage record type could
reach the shutdown path and call `exit(0)` from inside an analysis
routine -- every response read is checked, a failed one drops the
channel (`Sift::Writer::channelLost`), and the recorder ignores
`SIGPIPE`.  The magic L2 counters read zero because the cache controller
registers its counters per block type (`loads-data`, ...); they now sum
those.  `run-sniper --pacsim` no longer forces the pre-ROI region into
fast-forward (a cold ROI start put every first-touch miss into the first
measured region).  And `SIM_CMD_INSTRUMENT_MODE` switched only half of
what a mode switch is: what the trace thread does with each record is
one state, and whether the cores' performance models, the fast-forward
manager and the barrier are in fast-forward is another, moved together
only on the ROI path.  Switched alone inside the ROI, a fast-forward
mode left the manager off and the barrier in detailed mode: no core's
clock advanced, cores already waiting in the barrier stayed parked, and
the one thread whose trace thread never enters the barrier in that mode
ran a whole region alone -- the loop shape's estimate was a third high
for it.  The magic moves the second state with the same pair the ROI
toggle uses.  And in the recorder's memory-only mode a stretch
without a memory access sent nothing, so the simulator's clock for that
core stood still while the thread ran on natively and, the barrier being
active in warm-up, every other core waited for it; the pending count now
goes out as an instruction count once it outgrows the flow-control
window.

**Known limitation.**  The task-queue shape hands each task to whichever
thread asks first, so its schedule depends on timing, and the
fast-forward model's timing is not the detailed model's: a sampled run
executes a different schedule from the full run, and at the quick size
(36 irregular tasks, where one 1.5M-instruction task landing on another
thread moves the critical path by a tenth) the full run's time is not a
fixed truth to estimate -- the sampled run's own elapsed time differs
from it by about as much as the estimate does.  The same queue also
repeats too little at that size for the sampler to fast-forward more
than a region or two, so it is not faster there either.  Both checks
are listed in `testing/test_pacsim.py` as known quick-profile failures:
they run and are reported, and a pass is reported as such, but they do
not fail the suite.  The barrier and loop shapes, whose schedules do not
depend on timing, are within 2%.

**Known issue.**  A workload whose worker threads exit while the main
thread goes on running (`testing/apps/phased.c` and its siblings: the
workers leave at ROI end while main sums and prints) dies inside SDE in
about half of its runs, shortly after ROI end, with an invalid access in
SDE's per-thread emulation state or in Pin's instrumentation playback
(`/tmp/debug_backtrace_*.out` now carries the process map, which is how
this was attributed; handing `xsave` to the host, dropping the
instrumentation removal at ROI end and the recorder fixes above do not
change it).  The simulation's results are complete by then; what is lost
is the application's output after the ROI.  The three Pac-Sim workloads
therefore print their checksum inside the ROI, and a workload whose
output matters should join its workers before `SimRoiEnd()` or print
before it.

### 3.8 Tests

| suite | command | what it holds | size |
|---|---|---|---|
| scripting | `testing/test_scripting.py` | six Python-hook cases on a threaded workload (`statscb`, `periodic-stats`, `ipcthreadtrace`, `markers`, `no-script`, `static-scheduler`) | 2 minutes |
| configs | `testing/test_configs.py` | every shipped configuration and fragment at 1, 2 and 4 cores in fast-forward and detailed mode, each required to produce the workload's checksum; unclassified configurations are a failure | 318 checks, over an hour |
| qemu | `testing/test_qemu.py` | the QEMU frontend: x86 user traces, multi-threaded streams, an exact full-system count, VA-to-PA, mode switches; and the 21 RISC-V cases | hours with the RISC-V cases |
| pacsim | `testing/test_pacsim.py [--profile quick]` | three region shapes, each: same program, faster, agrees within tolerance; one core per thread, quick or full sizes (section 3.7) | minutes (quick) to an hour (full) |
| apps | `testing/test_apps.py` | every `test/` workload's `make run` with a check on what came out (the FFT's own verdict, MPI's pi, DVFS read-back, SMC's rewritten code, fork, spin loops) | 10 minutes |
| virtuoso | `testing/test_virtuoso.py` | four invariance configurations against host-local goldens (`--bless` once; ASLR pinned with `setarch -R`, a fixed environment and workload path) and 25 feature-activation cases, one per landed Virtuoso feature | 5 minutes |
| MMU runner | `testing/run_tests.py --suite smoke_test` | trace-driven MMU experiments from `testing/test_config.yaml` (needs the trace directories it names) | |

`make regress` runs the six suites in turn, keeps timings in one log and
prints where the wall clock went (`testing/regress_report.py`);
`FAILFAST=ON` stops at the first failure.

The feature cases and what each asserts: `dram_queueing` (the
per-stage DDR delays accumulate), `radix_fault_split` (prefetch faults
counted apart), `revelator` (the engine speculates: `total_prefetches`,
hash tries and hits), `victima` (leaf probes and hits), `hw_fault`
(faults handled in hardware with a latency), `os_daemons` (kcompactd and
khugepaged scans), `trail`, `berti`, `atp`, `recency`, `dp` (each
prefetcher predicts and its prefetches succeed), `tlb_prefetch_stress`
(sub-line credit within bounds), `l2_tlb_latency` (L2 TLB hits cost
cycles), `nonpow2_tlb` (a 1536-entry TLB runs), `magic_reserved` (the
eight reserved commands return), `roi_barrier` (the per-thread barrier
gates), `fork_child` and `sift_child_app_id` (children traced),
`mimicos_event_queue`, `app_reader_pool`, `fast_fault` (the seven-word
payload and its cost bucket), `mimicos_pcp_allocator` (fast and slow
allocation paths), `mimicos_scheduler` (round-robin quanta),
`mimicos_two_app_smp` (two applications on two kernel workers to
shutdown), `live_feed` (the feed and a dashboard frame),
`oversubscription` (eight threads on four cores finish), `numa_revelator`
(the per-node engine trains, hints and prefetches), `virt` (both stages
of the nested walk run), and the cases for the variants nothing selected
before: `stride` (the stride prefetcher's walks resolve),
`revelator_open_addressing` (probe 0 and probe 1 issued, probe 0 hits),
`revelator_thp` (2MB predictions issued and hit, page-table predictions
issued), `revelator_simple_allocator`, `revelator_thp_allocator` and
`numa_reserve_thp` (each allocator's own hash, 2MB and page-table
counters), `page_size_prediction` (the L2 latency is exactly the
predicted-hit and miss charges), `no_pwc` (walks charged with no page
walk cache registered), `mimicos_baseline_allocator` (every fault
unclassified under the plain buddy) and `mimicos_one_app_sched` (the
application taken from the kernel's `[apps]` list on one kernel core).

---

## 4. Running it

### 4.1 Environment

```bash
# verify:env
export SNIPER_ROOT=$(pwd)
WORK=${WORK:-/tmp/sniper-stack-examples}; mkdir -p $WORK
gcc -O1 -pthread -I include -o $WORK/workload testing/apps/workload.c    # the configs and Virtuoso suites' workload: prints a checksum
```

### 4.2 Build

```bash
# verify:build
make -j$(nproc)                          # fetch dependencies (once, into the shared cache), configure, build
python3 deps/sniper-deps status | tail -5
cmake --list-presets
ls -l lib/sniper build/default/bin/startup_mimicos
```

### 4.3 A first simulation

The SPLASH-2 FFT with its own self-check, four cores, detailed only in
the region of interest; then the statistics and the CPI stack:

```bash
# verify:first
make -C test/fft > /dev/null
./run-sniper -n 4 -c gainestown --roi -d $WORK/fft -- test/fft/fft -p 4 -t | grep -E 'TEST|Simulated|IPC'
ls $WORK/fft/simulation
python3 tools/dumpstats.py -d $WORK/fft | grep -E '^performance_model\.(instruction_count|cycle_count)|^L2\.(loads|load-misses) '
python3 tools/cpistack.py -d $WORK/fft --simplified | head -12
```

Configurations layer left to right and any key can be overridden:

```bash
# verify:configs
./run-sniper -n 1 -c gainestown -c rob -c dram_configs/ddr5_4800 -g --perf_model/l2_cache/cache_size=512 -d $WORK/ddr5 -- $WORK/workload | grep -E 'workload isum'
grep -E '^ddr\.(page-hits|page-miss)-data |^dram\.reads ' $WORK/ddr5/simulation/sim.stats
./run-sniper -n 1 -c gainestown -c cxl_configs/cxl -d $WORK/cxl -- $WORK/workload | grep -E 'workload isum'
grep -E '^cxl\.(reads|writes) ' $WORK/cxl/simulation/sim.stats
./run-sniper -c address_translation_schemes/multicore/parametric_baseline_4c -d $WORK/p4c -- $WORK/workload | grep -E 'workload isum'
grep -E '^mmu\.(num_translations|page_faults|page_table_walks) ' $WORK/p4c/simulation/sim.stats
```

### 4.4 Traces: record, inspect, replay

```bash
# verify:traces
./record-trace -o $WORK/fft-trace --roi -- test/fft/fft -p 1 -t | tail -1     # a stored trace: one file, no app/thread suffix
ls -l $WORK/fft-trace.sift
build/default/lib/siftdump $WORK/fft-trace.sift | head -5
./run-sniper -n 1 -c gainestown -d $WORK/fft-replay --traces=$WORK/fft-trace.sift | grep -E 'Using trace|Simulated'
```

A ChampSim trace runs the same way; the reader detects the format from
the extension.  (Not in the verification pass: this host has no ChampSim
trace.)

```bash
./run-sniper -n 1 -c gainestown -d out --traces=/path/to/delta_0002.champsim.gz
```

### 4.5 The QEMU frontend

```bash
# verify:qemu
make -j$(nproc) QEMU=ON > /dev/null                        # QEMU 11.1.1 with plugins and lib/libsniper-qemu.so
make -s -C test/qemu-plugin-probe QEMU_SRC=$SNIPER_ROOT/qemu st boot.img probe.so
qemu/build/qemu-x86_64 -plugin build/default/lib/libsniper-qemu.so,output=$WORK/st -- test/qemu-plugin-probe/st 2>&1 | tail -1
./run-sniper -n 1 -c nehalem-lite -d $WORK/st-sim --traces=$WORK/st.app0.th0.sift | grep -E 'Simulated'
qemu/build/qemu-system-x86_64 -nographic -accel tcg -m 64 -drive file=test/qemu-plugin-probe/boot.img,format=raw,if=floppy \
   -device isa-debug-exit,iobase=0xf4,iosize=0x04 -plugin test/qemu-plugin-probe/probe.so,quantum=100000,lo=0x7c00,hi=0x7c1f \
   -no-reboot 2>&1 | grep range_insns      # the boot sector exits QEMU through the debug port; exactly 3,003,007 in [0x7c00,0x7c1f)
```

### 4.6 Address translation

Each scheme, with the counter that shows the feature working.  ASLR is
pinned so the numbers reproduce (`setarch -R`, as the suite does).

```bash
# verify:mmu
sim() { local name=$1; shift; setarch -R ./run-sniper -n 1 "$@" -d $WORK/$name -- $WORK/workload > $WORK/$name.out 2>&1 || echo "$name: run-sniper exited $?"; grep -q 'workload isum=1419064784' $WORK/$name.out || echo "$name: no checksum"; }
stat() { grep -E "^($2) " $WORK/$1/simulation/sim.stats | sed "s/^/$1: /"; }
sim baseline -caddress_translation_schemes/baseline;              stat baseline 'mmu\.(page_faults|page_table_walks|tlb_misses_data)'
sim trail -caddress_translation_schemes/temporal_pte_stress;      stat trail 'tlb_temporal_pte\.(predictions_issued|prefetch_successful|demand_hit_inflight)'
sim berti -caddress_translation_schemes/baseline_berti;           stat berti 'tlb_berti\.(strides_learned|successful_prefetches)'
sim atp -caddress_translation_schemes/atp_stress;                 stat atp 'tlb_atp\.(real_prefetch_walks_issued|real_prefetch_successful)'
sim revelator -caddress_translation_schemes/revelator;            stat revelator 'revelator\.total_prefetches|revelator_allocator\.hash1_hits'
sim victima -caddress_translation_schemes/victima;                stat victima 'mmu\.(victima_leaf_probes|victima_leaf_hits)'
sim hwfault -caddress_translation_schemes/hw_fault;               stat hwfault 'mmu\.(page_faults_hw_handled|page_faults_os_handled)'
sim daemons -caddress_translation_schemes/os_daemons;             stat daemons 'kcompactd\.scans|khugepaged\.scans'
sim nonpow2 -caddress_translation_schemes/nonpow2_tlb;            stat nonpow2 'mmu\.(tlb_hits_data|tlb_misses_data)'
```

### 4.7 The userspace MimicOS kernel

One application from its trace, faults served by the kernel's buddy
allocator with per-CPU page pools (fast and slow allocation paths in the
stream), then the round-robin scheduler with instruction quanta, then two
applications on two kernel workers to shutdown:

```bash
# verify:mimicos
setarch -R ./run-sniper -n 2 -caddress_translation_schemes/userspace_baseline -d $WORK/mimicos -- \
   build/default/bin/startup_mimicos mimicos/configs/linux_buddy_anon_32GB.ini test/sniper-in-sniper/fft.sift $WORK/mimicos-report.txt > $WORK/mimicos.out 2>&1
grep -c 'alloc_path=fast' $WORK/mimicos.out; grep -c 'alloc_path=slow' $WORK/mimicos.out
grep -m2 '\[kernel\]' $WORK/mimicos.out | cut -c1-140
setarch -R ./run-sniper -n 2 -caddress_translation_schemes/userspace_baseline -d $WORK/mimicos-rr -- \
   build/default/bin/startup_mimicos mimicos/configs/linux_buddy_anon_sched_rr.ini test/sniper-in-sniper/fft.sift $WORK/mimicos-rr-report.txt > $WORK/mimicos-rr.out 2>&1
grep -E 'Scheduler\] Configured' $WORK/mimicos-rr.out | cut -c1-140; grep -E '^trace_thread\.quanta_expired ' $WORK/mimicos-rr/simulation/sim.stats
setarch -R ./run-sniper -n 4 -caddress_translation_schemes/userspace_baseline -d $WORK/mimicos-smp -- \
   build/default/bin/startup_mimicos mimicos/configs/two_app_smp.ini test/sniper-in-sniper/fft.sift $WORK/mimicos-smp-report.txt > $WORK/mimicos-smp.out 2>&1
grep -c 'shutdown event received' $WORK/mimicos-smp.out; grep -E 'Scheduler stats' $WORK/mimicos-smp.out | cut -c1-140
```

### 4.8 The run stream and the live feed

```bash
# verify:live
head -3 $WORK/mimicos-smp/simulation/sim.stdout | cut -c1-120          # stamped, tagged, one line per write
setarch -R ./run-sniper -n 4 -caddress_translation_schemes/userspace_baseline -s live:50000 -d $WORK/live -- \
   build/default/bin/startup_mimicos mimicos/configs/two_app_smp.ini test/sniper-in-sniper/fft.sift $WORK/live-report.txt > /dev/null 2>&1
head -c 300 $WORK/live/simulation/sim.live; echo
tools/sniper-live --once -W 120 -H 30 --log $WORK/live/simulation/sim.stdout $WORK/live/simulation/sim.live | head -30
```

`run-sniper --live -n 4 ... -- app` opens the same dashboard in the
terminal while the run goes; `tools/sniper-live <dir>` tails a running
one.  The stream in lnav (not in the verification pass: `lnav` is not
installed on this host):

```bash
lnav -I tools/lnav $WORK/mimicos-smp/simulation/sim.stdout
```

### 4.9 Pac-Sim

A sampled run next to the full one, on the barrier workload the suite
uses (`testing/apps/phased.c`, quick size: twelve phases alternating a
pointer chase with arithmetic, four workers, one core per thread with the
main thread's).  The quick size's whole ROI is under ten million
instructions, so the sampler's knobs are scaled with it, as
`testing/test_pacsim.py` does (regions of 1M instructions rather than 50M,
none under 100K, and a fast-forward flow-control window of 1,000
instructions rather than 100,000, so that a thread cannot run a tenth of
a region ahead of the simulator between measured regions).  The run
prints the regions it measured and the estimate; the full run's own
elapsed time is the truth to compare with.

```bash
# verify:pacsim
gcc -O1 -pthread -I include -DNPHASES=12 -DCHASE_SLOTS=131072 -DCHASE_STEPS=60000 -DCOMPUTE_ITERS=120000 -o $WORK/phased testing/apps/phased.c
./run-sniper --roi -n 5 -c gainestown -d $WORK/pacsim-full -- $WORK/phased > $WORK/pacsim-full.out 2>&1; grep -o 'phased: .*' $WORK/pacsim-full.out
./run-sniper --roi -n 5 -c gainestown --pacsim --trace-args='-sniper:pacsim-region-size 1000' --trace-args='-sniper:pacsim-min-region-size 100' --trace-args='-sniper:flowff 1000' \
   -d $WORK/pacsim-sampled -- $WORK/phased > $WORK/pacsim-sampled.out 2>&1; grep -o 'phased: .*' $WORK/pacsim-sampled.out
grep -o '\[PacSim\] [0-9]* regions.*cut points' $WORK/pacsim-sampled.out; grep -o 'Estimated sim time [0-9.e+-]*s' $WORK/pacsim-sampled.out
python3 -c "import re;print('full run: %.6gs' % (int(re.search(r'^performance_model.elapsed_time = (\d+)', open('$WORK/pacsim-full/simulation/sim.stats').read(), re.M).group(1))/1e15))"
python3 -c "import json;r=json.load(open('$WORK/pacsim-sampled/pacsim_regions.json'))['feature_stack'];print('%d regions, %d measured' % (len(r), sum(f['feature_level']==1 for f in r)))"
grep -E 'Elapsed time' $WORK/pacsim-full.out $WORK/pacsim-sampled.out | sed 's/.*\[SNIPER\] //'
```

**With Virtuoso.**  Sampling composes with every address-translation
scheme: the scheme's counters accumulate in both the measured and the
warm-up regions (the memory-only mode drives the MMU, the TLBs and the
page-table walks like the detailed mode does), so a sampled run exercises
the feature the same way and reports the same feature counters as the
full run, while the estimate covers the whole ROI.  On this quick
workload the ROI is a small part of the run under the Virtuoso
configurations (the pre-ROI warm-up of a hundred thousand page mappings
dominates), so the wall-clock gain is small there; the gain is in the
ROI.  The command is the same with the scheme's configuration added:

```bash
# verify:pacsim-virtuoso
for s in baseline revelator victima temporal_pte_stress hw_fault; do
  setarch -R ./run-sniper --roi -n 5 -c gainestown -caddress_translation_schemes/$s --pacsim --trace-args='-sniper:pacsim-region-size 1000' --trace-args='-sniper:pacsim-min-region-size 100' --trace-args='-sniper:flowff 1000' \
     -d $WORK/pacsim-$s -- $WORK/phased > $WORK/pacsim-$s.out 2>&1 || echo "$s: run-sniper exited $?"
  echo "$s: $(grep -o 'phased: .*checksum=[0-9.]*' $WORK/pacsim-$s.out | head -1 | sed 's/.*checksum=//') $(grep -o 'Estimated sim time [0-9.e+-]*s' $WORK/pacsim-$s.out)"
done
grep -E '^(mmu\.page_table_walks|revelator\.total_prefetches|mmu\.victima_leaf_hits|tlb_temporal_pte\.prefetch_successful|mmu\.page_faults_hw_handled) ' $WORK/pacsim-{baseline,revelator,victima,temporal_pte_stress,hw_fault}/simulation/sim.stats | sed "s#$WORK/pacsim-##"
```

The userspace MimicOS kernel (section 4.7) is not a candidate for
sampling: the recorder samples the process it runs, and that process is
the kernel replaying a stored trace, not the application.

The suite (`testing/test_pacsim.py`, section 3.8) does the comparison for
three region shapes -- barriers, loops only, a task queue -- and checks
that both runs printed the same checksum, that the sampled run was
faster, and that the estimate is within 10% of the full run; the two
task-queue checks that fail at the quick size for the reason in section
3.7 are reported as known failures.7 are
reported as known failures.

### 4.10 The regression suites

The Virtuoso goldens are host-specific: bless them once at a known-good
commit, then every later run must be clean.

```bash
# verify:long:suites
testing/test_virtuoso.py --bless > /dev/null       # once per host, at a known-good commit
make regress_scripting
make regress_virtuoso
make regress_configs
make regress_apps
make regress_pacsim
make regress_qemu
```

`make regress` runs all six and prints the timing report;
`SNIPER_REGRESS_LOG=<file> make regress` keeps the timings for
`testing/regress_report.py <file>`.

---

## 5. Verification record

Re-run on 2026-10-06 against commit `1dcf651` of `dev-mimicos-v2-update`
(the release cleanup series) on the same 80-core Debian 12 host (gcc
12.2, Python 3.11, SDE 10.13.1, MPICH 4.2.3 from the dependency cache),
the same way as before: every `# verify:` block extracted in order into
one script (`make doc-verify`), the `env` block sourced, every other
block in its own subshell with tracing on, its exit status recorded and
its output read.  Every block passed.

| block | result |
|---|---|
| `env`, `build` | configure and build (dependencies from the shared cache); `deps/sniper-deps status` reports every dependency present |
| `first` | the FFT prints `TEST PASSED`; statistics and the CPI stack read back |
| `configs` | DDR5, CXL and the four-core scheme run with the workload's checksum; `ddr.*` (3,442 page hits), `cxl.*` and `mmu.*` (91 faults, 182 walks) counters present |
| `traces` | a stored trace of 2.4 M instructions recorded, dumped and replayed (1.5 M cycles, 1.58 IPC) |
| `qemu` | user-mode trace of 32.1 M instructions simulated; the full-system probe counts exactly 3,003,007 |
| `mmu` | every scheme runs and its feature counter is non-zero: baseline 91 faults; TRAIL 14 predictions, 112 successful prefetches; Berti 82 strides; Agile 32 prefetch walks; Revelator 91 prefetches, 97 hash hits; Victima 91 probes, 69 hits; hardware faults 91 handled in hardware, 0 by the OS; the daemons 3,358 scans each |
| `mimicos` | the kernel's allocation counters, the round-robin scheduler's 115 expired quanta, two applications on two workers with their shutdown events and scheduler statistics printed |
| `live` | the stream stamped and tagged; the feed's header record (4 cores, 50,000 ns interval); a dashboard frame with the CPI stack, the cores and the kernel's log lines |
| `pacsim` | both runs print `checksum=59.570582 visited=1512832`; the sampled run cut 13 regions and measured 6; estimate 7.448 ms against the full run's 7.476 ms (0.4%) |
| `pacsim-virtuoso` | every scheme's sampled run prints the full run's checksum (59.570582) and an estimate (baseline 2.99 ms, Revelator 3.01 ms, Victima 2.99 ms, TRAIL 3.17 ms, hardware faults 2.99 ms); the schemes' counters accumulate in the sampled runs |
| `long:suites` | see below |

The earlier record (2026-09-20/21, against `344f0d5` and `e3a5ca0` of
`dev-mimicos-v2`) is superseded by this one; the series between the two
is the release cleanup (phases 1 to 7 in the commit history), the
Revelator exception handler, the virt MMU and the NUMA Revelator.

Corrected during the earlier pass (each re-run to a pass): the stored-trace
example named the file with an application and thread suffix, which only
live recording uses (a standalone `record-trace` writes `<prefix>.sift`);
the full-system probe example lacked the `isa-debug-exit` device the boot
sector uses to stop QEMU, so QEMU never exited.  Found and fixed in the
tree during the pass: the attached matrix tile read its coupling key
without a default, which aborted every non-RISC-V configuration on the
out-of-order core model (commit `2f7ba2a`); the configuration sweep
listed the RISC-V configurations as unbuildable and two fragments as
complete when they are not (commit `344f0d5`); and the hybrid MPI/OpenMP
workload never finished because an OpenMP worker parked in an emulated
futex held the process at exit (the fix follows this pass).

Not run here: the ChampSim replay (no ChampSim trace on this host) and
the lnav view (`lnav` not installed).

`long:suites`, after blessing the Virtuoso goldens on this host:
scripting 6 of 6, virtuoso 30 of 30 (20 minutes), configs 468 of 468
(144 minutes), apps 16 of 18 at the time (`mpi-omp` timed out -- the exit hang,
fixed since; `pinplay` replayed its pinball without any application
output -- the case judged a replay by output a replay does not produce,
fixed since), pacsim skipped as the recorder side was absent then;
after the Pac-Sim work and the recorder and simulator fixes it brought
(section 3.7), including the instrumentation-mode switch that now moves
the fast-forward state with it: apps 18 of 18, scripting 6 of 6,
virtuoso 30 of 30, configs 468 of 468, qemu 6 x86 cases with the 21
RISC-V cases skipped as before, pacsim quick: barrier all three checks
(0.3% error, 1.23x), loop all three (2.4% error, 1.28x), task same
program with the estimate and the speedup listed as known failures at
this size (section 3.7; 16% error and 1.05x in that run); qemu 6 x86 cases passed with the 21 RISC-V cases
skipped because that block does not set the RISC-V environment (they
are the companion document's `long:ime-check`, 19 of 19).
