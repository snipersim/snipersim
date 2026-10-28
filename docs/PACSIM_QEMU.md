# Pac-Sim on the QEMU front end

Pac-Sim samples a simulation online: it cuts the run into regions at loop
back-edges and synchronisation points, clusters the regions by their
basic-block mix, simulates each cluster in detail until its time per
instruction is known, and fast-forwards the rest, extrapolating their time.
The sampler was written against the SDE/Pin recorder; this document records
how it runs under the QEMU front end, what the port changed in the
sampler itself, and what it measured on the RISC-V agent.  The proposal
this work started from is `PACSIM_QEMU_PROPOSAL.md`; its design stands
except where noted.

## 1. Running it

The AME build of the agent (`test/llm-agent/README.md`), with any clang
that has the riscv64 target and lld:

    clang --target=riscv64-linux-gnu -march=rv64gcv -mabi=lp64d -O2 -ffreestanding \
          -nostdlib -static -fuse-ld=lld -Wl,-e,_start -DUSE_AME \
          -I test/llm-agent -I include -o test/llm-agent/agent-ame test/llm-agent/agent.c

then

    run-sniper --roi --pacsim --frontend=qemu -n 1 -c riscv \
        --trace-args="-sniper:pacsim-region-size 300" \
        --trace-args="-sniper:pacsim-warmup 0" \
        -d out -- test/llm-agent/agent-ame

`--frontend=qemu` picks `qemu-x86_64`, `qemu-aarch64` or `qemu-riscv64`
from the guest's ELF; `--pacsim` needs `--roi` and a live channel, which is
user mode (record-trace refuses the system-mode front ends).  The knobs are
the recorder's, spelled `-sniper:pacsim-<name> <value>` in `--trace-args`;
record-trace passes them to the plugin as `pacsim-<name>=<value>`.  At the
end run-sniper prints the estimate:

    [PacSim] 83 regions, 25 simulated in detail, 23 cut points; wrote out/pacsim_regions.json
    [PacSim] Estimated sim time 0.0132486s, wall time 570.291s, L2 miss rate 0.0325

(the wall time is the host's, under whatever else it is running)

and `out/pacsim_regions.json` holds every region: its instruction count,
block vector, cluster, cut address, whether it was measured, and its time.

| knob | default | meaning |
|---|---|---|
| `pacsim-region-size` | 50000 | thousands of instructions, summed over the threads, before a back-edge may end a region |
| `pacsim-min-region-size` | 2000 | a shorter region joins the next |
| `pacsim-cluster-threshold` | 0.2 | block-vector distance below which two regions share a cluster |
| `pacsim-min-measurements` | 2 | measurements of a cluster before its later regions are fast-forwarded |
| `pacsim-converge` | 0.10 | and only once the last two agree on the time per instruction to within this fraction (0: as soon as there are enough) |
| `pacsim-warmup` | 1 | between measured regions: 1 keeps the caches warm (cache-only records), 0 counts instructions only |
| `pacsim-warmup-window` | 0 | with warm-up: warm only the last K thousand instructions before a measured region (0: the whole fast-forwarded stretch) |
| `pacsim-loop-min` | 1 | shortest loop iteration whose back-edge may cut (1: any; 0: 1% of the region size) |
| `pacsim-markers` | cond | backward branches that may cut: conditional only, or `all` |
| `pacsim-syms` | | the guest's ELF, for the synchronisation points (`pthread_barrier_wait`, `GOMP_*`, `_omp_fn*`, `MPI_Barrier`); run-sniper passes the guest |
| `pacsim-sync` | | more synchronisation routines, by symbol name |
| `pacsim-dir` | . | where `pacsim_regions.json` goes |

## 2. What was built

**One sampler, two adapters.**  The sampler is in libsift
(`sift/pacsim_sampler.{h,cc}`, with the block-vector hash in
`sift/bbv_count.*`) behind a small front-end interface: is the thread in the
ROI, is its channel alive, ask the simulator a magic question, switch the
recording mode, flush a pending instruction count, log a line.
`sift/recorder/pacsim.{h,cc}` binds it to the Pin recorder (behaviour
unchanged; the Pin suite is the check) and
`frontend/qemu-plugin/pacsim_qemu.{h,cc}` to the plugin.

**Recording modes in the plugin.**  Every callback is registered once and
gated on a mode word: detailed writes instructions and memory operands;
memory-only writes `CacheOnly` records and reports the pending count every
200 instructions; counting reports every flow-control window; off records
nothing.  The mode comes from the sampler and from the answer the simulator
returns to `Sync()` and `InstructionCount()`, which the plugin now honours
as the recorder does.  No block is retranslated at a switch.

**Cut points.**  A translation block's last instruction is classified once:
x86 by XED, RISC-V and AArch64 from the encoding (B-type/JAL; B.cond/B/CBZ/
TBZ).  Backward direct branches get the back-edge callback; the entry of a
synchronisation routine, found in the guest's ELF, gets the sync-point
callback, keyed by the return address.

**Magic instructions and the ROI.**  `sim_api.h` defines one per ISA: x86
`xchg %bx,%bx`, AArch64 `bfm x0,x0,#0,#0`, RISC-V `slti zero,zero,0x5a`
(a hint no compiler emits), with the command and arguments in the first
argument registers.  The plugin recognises them at translation and sends
`Writer::Magic()` with the register values; on a live channel the answer is
written back to the result register, so `SimInSimulator()` and the sampler's
queries (`GET_SIM_TIME`, `GET_CORE_TIME`, `GET_L2_*`) work.  With `roi=1`,
`SimRoiStart()` turns recording on in detailed mode and `SimRoiEnd()` turns
it off; the guest runs on, the channel stays open so the magic instructions
after the ROI are still answered, and the simulator ends with the guest, as
under the other front ends.  A recorded trace keeps the markers outside the
ROI too.

**Simulator fixes found on the way.**  `BarrierSyncServer::setFastForward`
now raises the global time to the fast-forwarded core's when a core leaves
fast-forward, and `MagicServer::enablePerformance` takes its ROI-begin
snapshot after the models are enabled, so a reported ROI time no longer
carries up to one fast-forward quantum of slack.  (Neither moves the
Virtuoso invariance baselines: checked by reverting both against the
blessed counters.  The baselines did move with the sampler's move into
the recorder's shared object, because the live guest's address layout
follows the Pin tool's size; see testing/test_virtuoso.py.)

**Threads.**  A vCPU that waits for Sniper inside a plugin callback still
counts as running, so any exclusive section another thread needs waits for
it, and Sniper waits for that thread: a deadlock with two threads, which is
why the plugin refused a second one.  The wait the proposal asks for
(section 3.3, option 2) is `qemu_plugin_vcpu_wait_at()` in
`tools/qemu-11.1.1-plugin-wait.patch`, applied by `deps/sniper-deps` to the
x86-64, RISC-V (after the IME patch) and AArch64 user-mode QEMUs.  It is not
the proposal's begin/end pair: a callback cannot stop being "running" in
place, in the middle of a translated block a flush may reuse.  Instead it
leaves the run loop the way a system call does -- the state at the
instruction restored from the block's unwind data (x86's pending flags
operation included), the PC set to it, `EXCP_PLUGIN_WAIT` unwinding to the
linux-user cpu loop, where the plugin's callback runs after
`cpu_exec_end()` with register access -- and execution resumes at the
instruction.  The plugin (`sniper_qemu.cc`, "waiting"):

- flow control reports from the block-start callback, where leaving the
  loop re-executes nothing; a magic instruction, and a sampler event that
  would ask Sniper something (a cut at a back-edge or a synchronisation
  point, a pending count's flush), leave the loop and finish outside it.
  The sampler's events take `can_block` and report an event left undone;
- resuming re-runs the instruction's callbacks, in a fresh block starting
  there: every callback is a stage in a per-vCPU done-mask keyed by
  address, so none repeats, and a block begun by a resume is not counted
  twice;
- threads follow the recorder: the child-tid word queued at the clone's
  entry, `NewThread` at its successful return (glibc's `clone3` attempt
  gets ENOSYS first), the child waiting in its init hook until it has been
  announced, the tid word cleared and the joiner's futex wake sent to
  Sniper at the thread's exit call (QEMU's vCPU-exit hook runs under its
  clone lock); system calls are outside the run loop already, and the
  emulated ones are answered through the syscall filter as before;
- two orderings kept: the pending count goes out before `NewThread`, and an
  instruction whose own callback caused a request keeps its record pending
  until it has run, so the record carries its addresses and outcome.

The call is bound weakly: on a QEMU without it the plugin waits in the
callback as before and refuses a second thread on a live channel.  Two
faults of the sampler that only a deferred event could reach were fixed on
the way: the back-edge rule of `pacsim-loop-min` recorded the loop's
iteration on the first call, so the second call saw an iteration of no
instructions and never cut at a coarse loop (the x86 single shape went from
2 of 6 regions measured to 4 of 5, 2.3% off); and a warm-up window's start
counted its block twice.

**Not built.**  System-mode sampling: the wait is user mode only (the hook
is in the linux-user cpu loops), and sampling there needs guest-side
markers too.

## 3. What the port changed in the sampler

Measured on the agent (26M instructions a pass; 300K-instruction regions,
`pacsim-loop-min 0`, ten clusters).  What the sampler simulates in detail,
which does not depend on the host:

| sampler | 1 pass | 2 passes | 4 passes |
|---|---|---|---|
| as it was (whole-region warm-up) | 59% of the instructions, 49 of 83 regions | 40%, 67 of 166 | 21%, 72 of 332 |
| counting fast-forward | the same regions, without the warm-up's cost | | |
| plus shared clusters and convergence | 30%, 25 of 83 | 21%, 35 of 166 | 11%, 37 of 332 |

The speedup of the last row, by CPU time, with the full detailed run and the
sampled run side by side on the same host at the same time (config/riscv.cfg,
2026-10-25):

| | 1 pass | 2 passes | 4 passes |
|---|---|---|---|
| full run, CPU seconds | 218 (200 on the configuration before 9.0's fixes) | 461 | 890 |
| sampled run, CPU seconds | 75 (69) | 123 | 140 |
| speedup | 2.9x (2.9x); 2.6x in a second pair under heavier load | 3.7x | 6.4x |
| estimate against the full run | +0.34% (-0.07%) | +0.25% | +0.48% |

An earlier version of this table gave 4.3x, 6.2x and 10.3x.  Each full run
behind those was timed alone, under a much heavier host load than its sampled
run (287 CPU seconds for the one-pass full run, against 200 when the pair runs
together), which overstated every speedup by about 1.4x; CPU time on a shared
host is only comparable between runs made together.  The core configuration
does not move the speedup: on the configuration before 9.0's fixes and on the
present one the pair gives 2.9x alike.

The estimate is within 0.5% of the full run on every row.  Four things
limited the first row, in order of weight:

1. **Warm-up cost.**  Per simulated instruction the cache-only warm-up
   costs 0.57 of detailed simulation (counting 0.03), so warming every
   fast-forwarded region caps the speedup near 1.6x however long the run
   (cost ratios from runs made together).
   The agent's working set stays resident: without warm-up its estimate
   moved 0.03%.  `pacsim-warmup-window` is for workloads that do need it.
2. **Learning per cut point.**  The predictor learned clusters per cut
   address, and a compiler's token loop has many back-edges (23 in the
   agent's `generate()`), so each one re-measured every cluster twice: 49
   of 83 regions.  A cluster's measurements now count wherever they were
   taken, with a run-wide history for a cut point seen for the first time:
   23 of 83.
3. **Cold first regions.**  The first prompt runs at 0.99 fs per
   instruction, the later ones at 0.55, and the first measurement stood for
   the cluster.  `pacsim-converge` keeps measuring until two agree.  On the
   suite's task shape, whose clusters rarely agree, it then measures every
   region and the estimate is right (0.2%, from 36%) with the run no shorter;
   when two measurements of a cluster happen to agree, that cluster's later
   regions are skipped and the error is 15-30%, so the shape's accuracy check
   stays a known failure of the quick profile.
4. **The reported estimate re-extrapolated in Python.**
   `tools/pacsim/pacsim_dumpjson.py` refilled every unmeasured region from
   the regions measured *before* it, so the cold regions stood for their
   cluster (5.6% error where the sampler's own solution was within 0.2%).
   The sampler now writes `"solved": true` into the region file and the
   estimate uses its times; a file without the flag (the legacy recorder's)
   is extrapolated as before.

Tools whose code never repeats are the case sampling cannot shorten.  With
each tool call running its own split of four kernels (`TOOL_WORK` in
`test/llm-agent/agent.c`, 33% to 66% of the run), the sampler extrapolates
them from the nearest measured cluster: the error grows to about 1% and the
speedup is 3.8x (tools a third of the run, +1.06%) to 5.8x (two thirds,
+1.12%), each pair measured together.

## 4. Tests

- `testing/test_pacsim.py --frontend qemu --arch x86_64|riscv64|aarch64
  --profile quick`: the single-threaded phased shape under the plugin, and
  with the wait call the multi-threaded ones (four workers, one core per
  thread): on x86-64 `barrier`, `loop` and `task` built statically, on
  RISC-V and AArch64 `barrier` from `phased_rv_mt.c`.  Measured 2026-10-28
  (error against the full run; wall-clock speedup, runs pinned to eight
  cores that other runs shared):

  | shape | x86-64 | RISC-V | AArch64 |
  |---|---|---|---|
  | single | 0.20%, 1.69x | 0.05%, 1.63x | 0.03%, 2.11x |
  | barrier | 0.09%, 1.60x | 0.44%, 1.59x | 0.50%, 1.98x (with `pacsim-converge 0.05`; at the default 0.10, 20.7%: two cold measurements agreed, see the suite) |
  | loop | 0.03%, 2.35x | | |
  | task | 24.6% (the known exception), 1.39x | | |

  `--frontend pin` (the default) is the recorder suite as before.
- `testing/test_qemu.py`: `user-pacsim`, `riscv-user-pacsim`,
  `aarch64-user-pacsim` run the single shape through run-sniper, and
  `user-pacsim-mt`, `riscv-user-pacsim-mt`, `aarch64-user-pacsim-mt` the
  barrier shape (skipped on a QEMU without the wait call);
  `user-mt-live`, `riscv-user-mt` and `aarch64-user-mt` run a
  multi-threaded guest live and check its checksum (or, without the call,
  the refusal); `run-sniper-qemu-roi` checks the ROI and the magic answers
  on the three architectures.
- `testing/apps/phased_st.c`, `phased_rv.c`, `phased_rv_mt.c`: the shapes,
  freestanding so they build for every architecture with the pinned clang.
