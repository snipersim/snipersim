# Pac-Sim on the QEMU front end: proposal

Status: the proposal this work started from, kept as written.  What was
built, and where it departs from this, is in `PACSIM_QEMU.md`.

## 1. The question

Pac-Sim's recorder side lives in the SDE/Pin recorder
(`sift/recorder/pacsim.{h,cc}`).  Can the QEMU front end
(`frontend/qemu-plugin/sniper_qemu.cc`, a TCG plugin that writes SIFT)
run sampled too, for x86 and RISC-V, and what does that take on the QEMU
side?

Short answer: yes for both architectures, with the sampler itself moved
into a front-end-independent library and a modest amount of plugin work.
Single-threaded guests need no QEMU change at all.  Multi-threaded
guests need one small addition to the plugin API, in the patch this
branch already carries, because a vCPU that waits for Sniper inside a
plugin callback deadlocks QEMU's exclusive sections.  That limit is not
Pac-Sim's; it is why the plugin refuses a second thread in user mode
today.

## 2. What Pac-Sim needs from a front end

The sampler is a state machine over six primitives.  Everything else
(clustering, prediction, extrapolation, the region file) is arithmetic
on what they deliver.

| need | in the Pin recorder | in the QEMU plugin today |
|---|---|---|
| count every basic block per thread, with a block vector | `BBL_InsertCall` on every block | per-TB exec callback exists (`vcpu_tb_exec`), counts instructions; no block vector yet |
| loop back-edges as cut points | tail branch with target below its address | x86: XED classifies the tail already; RISC-V: decode B-type/JAL from the encoding, trivial |
| synchronisation points as cut points | `RTN` callbacks on `pthread_barrier_wait`, `GOMP_*`, `_omp_fn`, `MPI_Barrier` | user mode: `qemu_plugin_insn_symbol()` gives the symbol at translation; system mode: no symbols, loop cuts only (or a guest-side marker) |
| three instrumentation modes: detailed, memory-only, count-only | mode-specific instrumentation plus `PIN_RemoveInstrumentation` | one mode: always detailed; the mode Sniper returns from `Sync()` is ignored |
| ask the simulator: `INSTRUMENT_MODE`, `GET_SIM_TIME`, `GET_CORE_TIME`, `GET_L2_*` | `Sift::Writer::Magic` over the response channel | the plugin links the same `Sift::Writer`; `Magic()` is available; a response channel exists in user mode only (`response` option) |
| the ROI: start sampling at ROI begin, finish and write at ROI end | `beginROI`/`endROI` hooks | no magic-instruction detection at all: no ROI, whole program always |

So the plugin is missing four things: the block vector, mode switching,
magic instructions, and a sampler.  The first three are plugin work; the
sampler should not be written twice.

## 3. Design

### 3.1 One sampler, two front ends

Split `sift/recorder/pacsim.cc` into:

- `sift/pacsim_sampler.{h,cc}`: `Region`, `Clusterer`, `History`,
  `Predictor`, `Sampler` with its cut, predict, measure, repair and
  write logic.  Pin-free: it takes a small interface,

      struct PacSimFrontEnd {
         virtual bool     inRoi() = 0;
         virtual bool     channelAlive(unsigned tid) = 0;
         virtual uint64_t magic(unsigned tid, uint64_t cmd, uint64_t arg) = 0;
         virtual void     setMode(Mode m) = 0;          // detailed | memory | icount
         virtual void     flushCounts(unsigned tid) = 0;
         virtual void     log(const std::string &) = 0;
      };

  and a plain `std::mutex` with `try_lock` in place of `PIN_MUTEX`.
- `sift/recorder/pacsim.cc` becomes the Pin adapter: the six hooks above
  bound to `thread_data[]`, `setInstrumentationMode`, `in_roi`, and the
  Pin instrumentation callbacks.  Behaviour unchanged; the existing
  suite is the check.
- `frontend/qemu-plugin/pacsim_qemu.cc`: the QEMU adapter.

The knobs keep their names (`pacsim`, `pacsim-dir`, `pacsim-region-size`,
`pacsim-min-region-size`, `pacsim-cluster-threshold`,
`pacsim-min-measurements`, `pacsim-warmup`) as plugin options:
`-plugin sniper_qemu.so,output=…,response,pacsim,pacsim-dir=…`.
`record-trace` gains the same knob check for the QEMU front end that it
has for the recorder, so `run-sniper --pacsim` refuses a plugin built
without the sampler rather than tracing unsampled.

### 3.2 Plugin work, by primitive

**Block vector.**  The per-TB exec callback already has the TB's address
and instruction count; add a `Bbv` per vCPU (the same 16-dimension hash
as `sift/recorder/bbv_count`) and count there.  Cost: one hash and one
add per translated block, in every mode.

**Back-edges.**  At translation, classify the TB's last instruction.
x86: XED already decodes it for the branch flag; add "direct,
conditional, target below".  RISC-V: B-type and JAL are three fields in
the encoding.  Register the back-edge callback on that instruction only.

**Synchronisation points.**  User mode: at translation, if
`qemu_plugin_insn_symbol()` of the TB's first instruction is one of the
known names (`pthread_barrier_wait`, `GOMP_barrier`, `GOMP_parallel`,
`__kmp_barrier`, `MPI_Barrier`, or contains `_omp_fn`), register the
sync-point callback there, keyed by the return address read from the
stack (x86) or `ra` (RISC-V) through the register API the branch
already carries.  System mode has no symbols; regions come from
back-edges, and a guest can add its own cut points with
`SIM_CMD_MARKER` (already a magic) if it wants barrier alignment.

**Modes.**  Register every callback once and gate at run time on a
per-plugin mode word: detailed writes instructions and memory
operands as now; memory-only writes `CacheOnly` records with the pending
count (the writer has it, the format has it, the simulator's
`handleCacheOnlyFunc` consumes it) and reports the pending count as an
instruction count once it passes the flow-control window, exactly as the
recorder does since `e3a5ca0`; count-only accumulates and reports.  No
re-translation is needed, so no TB flush and no QEMU change; the price
is one predictable branch per callback.  The mode comes from two places,
as in the recorder: the sampler's own switch, and the mode the simulator
returns from `Sync()`/`InstructionCount()`, which the plugin starts to
honour.

**Magic instructions.**  Detect at translation and turn into
`Writer::Magic()` calls with the register values.  x86: `xchg %bx,%bx`
(`sim_api.h`), XED sees it.  RISC-V: there is no RISC-V `SimMagic` in
`sim_api.h` today; define one as an architecturally-neutral hint the
plugin recognises -- the existing insn-claim API is the natural hook, and
`slti x0, x0, <imm>` with the command in `a0`..`a2` is the conventional
shape.  With that, `SimRoiStart`/`SimRoiEnd`, the ROI toggle and the
sampler's queries all work on RISC-V, and the plugin gains an ROI (start
tracing in the requested mode at ROI begin, `exit_group`+`End` at ROI
end) that the RISC-V harness can use too.

**Time.**  `SIM_CMD_GET_SIM_TIME`, `GET_CORE_TIME`, `GET_L2_COUNT`,
`GET_L2_MISS_COUNT`, `INSTRUMENT_MODE` are simulator-side and already
front-end independent, including today's fix that moves the
fast-forward state with the mode.

### 3.3 The one real obstacle: waiting for Sniper inside a callback

Every synchronous question -- flow control (`Sync`), a magic reply, a
syscall's result -- blocks the vCPU thread inside a plugin callback until
Sniper answers.  QEMU counts that vCPU as running.  Any other vCPU that
needs an exclusive section (`start_exclusive`: TB flush, `mprotect`,
`clone`, many syscalls) waits for every running vCPU to reach a safe
point, and the blocked one never does; Sniper meanwhile will not answer
until the other vCPU delivers instructions.  That is the deadlock the
plugin refuses at startup, and the reason multi-threaded guests today
run only in full-system mode with QEMU owning time (no blocking, no
replies).  Pac-Sim needs replies.

Options, honestly:

1. **Single-threaded guests, user mode, no QEMU change.**  Everything
   above works as is.  Useful for validating the port, useless for
   Pac-Sim's purpose.
2. **A blocking wait that leaves the run loop.**  One plugin API call,
   `qemu_plugin_vcpu_wait_begin()`/`_end()`, that brackets the wait with
   what QEMU does around a syscall in linux-user (`cpu_exec_end` /
   `cpu_exec_start`, which also honour a pending exclusive section), and
   in system mode the equivalent of leaving the TCG loop
   (`qemu_mutex_unlock_iothread` is not enough on its own; the vCPU must
   be marked as not running for `start_exclusive`).  About thirty lines
   in `plugins/api.c` plus the two arch-independent helpers, fifth entry
   in `tools/riscv/qemu-11.1.1-ime.patch` next to the four this branch
   already adds (`insn_claim`, `raise_exception`, `register_vcpu_registers`,
   `sysreg`).  With it, multi-threaded user-mode guests and MTTCG
   full-system guests can both wait for Sniper.  This is the route I
   recommend: small, contained, and it also lifts the plugin's
   single-thread restriction for ordinary (unsampled) traces.
3. **Round-robin TCG (`-accel tcg,thread=single`) in system mode.**  No
   exclusive-section deadlock, but a blocked vCPU stops the round robin,
   so the other cores deliver nothing and Sniper's barrier never releases
   the answer.  Not viable without making flow control asynchronous,
   which lets the guest run ahead until the pipe is full and deadlocks
   there instead.

So: without any QEMU change, single-threaded only; with one small
plugin-API addition, the full thing.

### 3.4 x86 and RISC-V

The same plugin serves both; the architecture-specific parts are three
small tables:

| | x86 | RISC-V |
|---|---|---|
| magic instruction | `xchg %bx,%bx`, XED | new `SimMagic` hint, claim callback |
| back-edge | XED direct conditional, target < pc | B-type / JAL fields |
| return address at a sync point | stack, via register `rsp` | register `ra` |

Symbols, modes, block vectors, the sampler and every simulator-side
piece are shared.  RISC-V additionally already has the IME/AME claim
machinery in the plugin, which is unaffected: the sampler only adds
callbacks.

### 3.5 Tests

- `testing/test_pacsim.py --frontend qemu-user`: the same three
  workloads (`phased.c`, `looped.c`, `tasked.c`) built for x86 and for
  RISC-V (`riscv64-linux-gnu-gcc`, already used by the RISC-V harness),
  run under `qemu-x86_64` / `qemu-riscv64` with the plugin, the same
  three checks (same checksum, faster, within tolerance).  The barrier
  and loop shapes are the meaningful ones, as in the Pin suite.
- `testing/test_qemu.py`: two new cases, `user-pacsim` and
  `riscv-user-pacsim`, quick sizes, so `make regress_qemu` covers it.
- The single-threaded stage first: a one-worker `phased.c` proves modes,
  magic and the region file before the QEMU patch lands.

### 3.6 Order of work

1. Split the sampler out of the recorder; Pin suite unchanged (half a
   day, mostly moving code).
2. Plugin: block vector, back-edges, modes honoured from `Sync`, x86
   magic and the ROI.  Single-threaded `phased.c` sampled under
   `qemu-x86_64` against a full run (one to two days).
3. RISC-V `SimMagic` and the claim hook; the same for `qemu-riscv64`
   (half a day).
4. The plugin-API wait, in the QEMU patch; lift the single-thread
   refusal; multi-threaded `phased.c` on both architectures; the suite
   cases (one to two days, most of it in QEMU's exclusive-section
   semantics).
5. Sync-point symbols in user mode; the loop and barrier shapes at the
   Pin suite's tolerances (half a day).

What I would not do: system-mode Pac-Sim in the first pass.  It needs
step 4 plus guest-side markers to get barrier-aligned regions, and the
value is in user mode first.

## 4. Risks

- The plugin API's register access at a sync point costs a register
  read per barrier; negligible.
- Memory-only mode doubles the plugin's memory-callback cost relative to
  count-only; the recorder pays the same and it is the accurate choice.
- The exclusive-section change is the one piece touching QEMU proper.
  It must not be visible to plugins that never wait; a vCPU that never
  calls the new API behaves as before.
- RISC-V `SimMagic` encoding must be one no compiler emits: a `slti`
  writing `x0` is the accepted hint space.
