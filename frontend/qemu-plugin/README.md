# QEMU frontend

A Sniper frontend that loads into a **stock** QEMU as a TCG plugin. No QEMU
fork, no QEMU patches. It writes SIFT, so to Sniper it is another trace
producer.

```
make QEMU=ON -j8          # builds QEMU with plugins, then lib/libsniper-qemu.so
```

## Running

From run-sniper, like the other front ends:

```
./run-sniper -c gainestown --frontend=qemu -- ./app            # user mode, live
./run-sniper -c cortex-a72 --frontend=qemu --roi -- ./app-arm64
./run-sniper -c riscv      --frontend=qemu -- ./app-riscv64
./run-sniper -c riscv      --frontend=qemu-system-riscv64 -- -M virt -bios none -nographic -kernel ./guest
./run-sniper -c gainestown --frontend=qemu-system-x86_64 -- -drive file=disk.img,format=raw ...
```

`--frontend=qemu` is user mode: the program's ELF header picks the QEMU
(x86-64, AArch64 or RISC-V), and it runs live -- Sniper paces it and answers
its magic instructions and system calls -- so it takes single-threaded guests.
`--frontend=qemu-system-<arch>` runs a whole machine from QEMU's own options,
records each vCPU to a file in the output directory, then simulates the
files; a machine reports its own way out (a test finisher, isa-debug-exit),
so QEMU's exit status is printed rather than taken as failure.  `--roi`
traces only between `SimRoiStart()` and `SimRoiEnd()`.  The QEMUs are the
dependencies `deps/sniper-deps` builds (`qemu`, `qemu-aarch64-user`,
`qemu-riscv-ime`, `qemu-riscv-system`); `$SNIPER_QEMU_X86_64`,
`$SNIPER_QEMU_AARCH64`, `$SNIPER_QEMU_RISCV`, `$SNIPER_QEMU_SYSTEM_X86_64` and
`$SNIPER_QEMU_RISCV_SYSTEM` point at others.  `--frontend-option` passes QEMU
options (`--frontend-option='-cpu rv64,v=true'`).  What QEMU cannot do is
refused rather than ignored: `--pid`, pinballs, `--pacsim`, MPI, `-f/-d/-b`
instruction windows, `--vma`, and `--pa` in user mode.  Three options the
other front ends take are accepted and have no effect here, because run-sniper
passes them on every live run: `-e` (system calls are emulated by QEMU itself;
the simulator only times them), `--follow` (QEMU user mode runs one process)
and `--routine-tracing`.

By hand, user mode, single-threaded guest:

```
qemu-x86_64 -plugin build/default/lib/libsniper-qemu.so,output=trace -- ./app
./run-sniper -n 1 -c nehalem-lite --traces=trace.app0.th0.sift
```

Full system, with Sniper owning guest time:

```
qemu-system-x86_64 -accel tcg -plugin \
  build/default/lib/libsniper-qemu.so,output=trace,time,ns=1 ...
```

Options: `output=` prefix, `app=N` the application number in the file names
(`<prefix>.app<N>.th<vcpu>.sift`), `flow=N` instructions per quantum,
`response=on` (user-mode blocking flow control), `roi=1` (trace only inside
the region of interest), `smc=0` (send each code page once), `time`
(system-mode time control), `ns=N` nanoseconds per instruction,
`verbose=on`.

### Magic instructions

`include/sim_api.h`'s commands (`SimRoiStart()`, `SimMarker()`,
`SimGetProcId()`, `SimUser()`, ...) work on every architecture the plugin
traces.  Each marker is a no-op on hardware:

| Architecture | Instruction | Command | Arguments | Answer |
|---|---|---|---|---|
| x86-64 | `xchg %bx, %bx` | rax | rbx, rcx | rax |
| i386 | `xchg %bx, %bx` | eax | edx, ecx | eax |
| AArch64 | `bfm x0, x0, #0, #0` | x1 | x2, x3 | x0 |
| RISC-V | `slti zero, zero, 0x5a` | a0 | a1, a2 | a0 |

The RISC-V marker is an SLTI writing x0, a HINT the ISA designates for custom
use.  The plugin sends each one to Sniper as a SIFT magic record; in user mode
it writes Sniper's answer into the answer register before the instruction
runs, and serves Sniper's reads of guest memory (a string argument, an
emulated system call's buffers).  Full system has no response channel: the
record is a notice (`RecOtherMagicInstructionNoResponse`), acted on -- an ROI
marker switches the models -- but not answered, so a guest reads its command
back, as on hardware.  `testing/test_magic.py` checks every command on every
architecture and front end.

## RISC-V

The same plugin loads into a RISC-V QEMU.  It reads the target from QEMU at
install: for `riscv64`/`riscv32` the conditional-branch classification is a
few bit tests (the BRANCH major opcode and `c.beqz`/`c.bnez`), there is no
mode probing, and each stream opens with an `ISAChange` to `DL_ISA_RISCV` so
the reader decodes it as RISC-V.  Everything else -- deferred emission for
the taken flag, memory addresses from the memory callbacks, code bytes read
from guest memory for the icache -- is the same code as x86.

The simulator side needs the RISC-V core model (on by default; no external
dependency since the decoder is generated):

```
make QEMU=ON -j8
qemu-riscv64 -plugin build/default/lib/libsniper-qemu.so,output=trace -- ./app
./run-sniper -n 1 -c riscv --traces=trace.app0.th0.sift
```

The plugin is built against the QEMU under `qemu/`; any other QEMU of the
same plugin API version loads it -- a QEMU built from a patched tree for an
extension under development, for one.  An instruction the generated tables do
not know (an extension whose specification is not in the policy yet) is
reported once per encoding and costed as a generic instruction, so a trace
with a few of them still simulates; modelling it is a policy edit and a
regeneration (docs/RISCV_ISA_GENERATION.md).

`testing/test_qemu.py` has `riscv-user-trace` and `riscv-user-simulate`,
which take the QEMU and a RISC-V compiler from the environment and skip,
saying so, without them:

```
SNIPER_QEMU_RISCV=/path/to/qemu-riscv64 SNIPER_RISCV_CC=/path/to/clang \
  python3 testing/test_qemu.py --case riscv-user-trace --case riscv-user-simulate
```

## Which mode, and why it matters

|  | user mode | full system |
|---|---|---|
| pacing | `Sift::Writer::Sync()`, blocking | `qemu_plugin_update_ns()` |
| Sniper controls | thread order | time |
| guests | **single-threaded only** | multi-core |

Multi-threaded user mode is refused at startup rather than allowed to
deadlock. A vCPU blocked inside a plugin callback still counts as running, so
`start_exclusive()` -- used by `fork()` and by code-cache flushes -- waits for
it; if the timing model will not release that vCPU until another one advances,
nothing moves again. This is reproduced in `test/qemu-plugin-probe`.

Full system is not exposed to that, because Sniper tells QEMU what time it is
instead of holding a vCPU. QEMU keeps choosing the interleaving; Sniper
chooses time. That is the trade, and it is why full system is the default.

**MimicOS does not apply here.** MimicOS replaces the OS for user-space
simulation; under full system there is a real guest kernel owning page tables
and allocation, and QEMU reports the real translations. Enabling both would
mean two competing page-table models.

## Replacing QSim

This supersedes `frontend/qsim-frontend`, which was removed. QSim gave Sniper
full-system tracing and let it choose which vCPU advanced next -- `osd->run(i,
1)` in a single-threaded loop -- which is more control than this frontend has.
It was retired anyway:

- QSim's last upstream change was June 2019; its required QEMU fork
  (`gtcasl/qemu`, branch `qsim_v26`) last changed August 2018 and forks QEMU
  **2.6** (May 2016).
- That QEMU does not build on a current toolchain: `configure` refuses Python 3
  outright, and Debian 12 ships no Python 2.
- Its Sniper frontend had not compiled since the `frontend/` framework changed
  in October 2022.
- Porting it is a rewrite, not a rebase: its ~2,265 lines of instrumentation
  live in files that have all moved and been restructured upstream --
  `target-i386/translate.c` alone went from 8,367 lines to 4,094 in
  `target/i386/tcg/`.

What is lost is Sniper-chosen interleaving in full system. Recovering it needs
a small upstream QEMU change -- a way to mark a callback as "may block" so the
vCPU is treated as parked -- which is the same class of request as
`qemu_plugin_request_time_control()`, already accepted for this kind of tool.

## Validation

Against the DynamoRIO frontend on the same binary: first 40 instructions
identical, 32,049,895 instructions vs 32,057,806 (0.025%), simulated IPC 0.70
vs 0.72. Full-system instruction counting matches a hand-computed ground truth
exactly, and paged VA -> PA agrees with guest page tables. See
`test/qemu-plugin-probe`.

## Full system and CPU mode — the blocker for booting an OS

Full-system tracing works; **simulating** a full-system trace does not yet, and
the reason is CPU mode rather than anything about operating systems.

A machine starts in 16-bit real mode, moves through 32-bit protected mode and
reaches 64-bit long mode. Sniper's decoder is fixed at 64-bit
(`decoder_lib/x86_decoder.cc`), so it aborts on the first instruction that was
not encoded that way:

```
x86_decoder.cc:58: virtual void dl::X86Decoder::decode(dl::DecodedInst*):
Assertion `res_decode == XED_ERROR_NONE' failed.
```

Measured both ways, so the cause is not in doubt: an Alpine 6.6 kernel booted
with `-kernel` fails this, and so does the pure 16-bit `boot.img` from
`test/qemu-plugin-probe` — 9.6M instructions of real-mode code and nothing
else. Linux complexity is not the problem.

SIFT already has the mechanism: `Sift::Writer::ISAChange(uint32_t)`. Nothing
emits it and nothing consumes it. Closing this needs two pieces, and they are
the next real work on this frontend:

1. the plugin detecting a mode switch and emitting `ISAChange`, and
2. the decoder honouring it instead of assuming `XED_MACHINE_MODE_LONG_64`.

Until then, full-system tracing is validated by instruction counts and VA→PA
translation, not by simulating what it records.

### What a Linux boot actually costs

Alpine `vmlinuz-virt` 6.6.134 booted with `-kernel`, to the point it panics for
lack of a root filesystem:

| machine | instructions | wall (QEMU + counting plugin) |
|---|---|---|
| default `pc` | 896 M | 9 s |
| `-M microvm` | 858 M | 8 s |
| `-M microvm,acpi=off,pcie=off`, 128 MB | 1046 M | 10 s |

Direct kernel boot is what makes this quick — it skips SeaBIOS and the
bootloader entirely. `microvm` saves a further 4%; trimming memory and buses
made it *worse*, so measure rather than assume.

Two properties rule a Linux boot out as a regression test, independent of the
decoder problem:

- **It is not reproducible.** Two identical `microvm` runs differed by 289 K
  instructions, 0.03%; timer interrupts and entropy see to that. The
  hand-written guests give exactly 3,003,007 every time.
- **Recording is slow and large.** Writing SIFT rather than just counting, the
  same boot produced a 232 MB trace at about 22 MiB/min and had not finished in
  280 s. The trace write, not emulation, is the bottleneck.

A booted OS belongs in a manual fidelity check, and probably behind ROI markers
so only a window of interest reaches SIFT. It does not belong in `make regress`.

## is_branch means *conditional* branch

Narrower than "transfers control", and it matters. Both existing frontends
restrict it the same way -- Pin uses `INS_IsBranch(ins) && INS_HasFallThrough(ins)`,
DynamoRIO uses `instr_is_cbr(instr)` -- so `jmp`, `call` and `ret` are excluded
even though `decoder_lib`'s `is_branch_opcode()` lists them.

Marking an unconditional transfer aborts the simulation: it decomposes into
several micro-ops and the one at `exec_base_index` is not the branch, so Sniper
fails its own assertion, `*ERROR* Expected to find a branch here.`

The plugin decodes with XED at translation time and matches that set exactly.
Checked against the DynamoRIO frontend on the same binary: 5,241 shared
addresses, zero disagreements in either direction.

ARM guests will need the equivalent through Capstone; only x86-64 is handled
today.

## RISC-V vector and matrix (IME) instructions

Sniper's RISC-V decoder is generated from the RISC-V Unified Database
(`tools/riscv/gen_isa.py`, see docs/RISCV_ISA_GENERATION.md): the same
tables give every instruction, the vector extension and the Integrated
Matrix Extension (IME, Zvvm freeze 0.9) included, a class with flags.  The
plugin links those tables (`decoder_lib/riscv_isa*.cc`) and uses them for
two translation-time decisions: whether an instruction is a conditional
branch, and whether its memory footprint must be recorded (the `footprint`
flag of its class).

**Footprints.**  A trace instruction carries at most two data addresses,
which cannot describe a tile: a `vmtl.v` reads M rows of `linesize`
elements, each row wherever the leading dimension puts it.  For every
vector or tile memory instruction the plugin records all of its accesses
(QEMU reports each element) and coalesces them into contiguous byte
ranges, so a unit-stride load is one range and each row of a strided tile
is its own; the ranges are the exact bytes moved (two rows in one cache
line still cost one cache access: the core squashes the second uop).  The ranges go into the trace ahead of the instruction as a
`MemorySpans` record (`Sift::Writer::MemorySpans`, up to
`Sift::MAX_MEMORY_SPANS` = 32 per instruction) and the reader delivers
them with it (`Instruction::spans`).  An instruction that touches more
ranges than that, a gather over thousands of lines at a wide VLEN, is
coalesced by cache line instead; the first 32 lines are recorded and
timed, the lines beyond them are counted in the record
(`Instruction::spans_dropped_lines`, the statistic
`riscv.spans_dropped_lines`) so the footprint is still known; adjacent
lines fold into one span, so a gather over a contiguous region is one
sized access.  `test/riscv-vlen-probe/gather.c` and the harness case
`riscv-footprint-lines` check it at 4,096 and 65,536 bits.
`siftdump` prints them as `-- spans <addr>+<bytes><r|w>`.  A trace from
the Pin recorder never contains the record, and a reader that predates it
skips it.

**Simulation.**  The trace thread turns each range into one memory access
of that size (the core splits it over cache lines), so every line a tile
touches goes through the cache hierarchy and the ROB.  A vector or tile
memory instruction is given `perf_model/core/riscv/memory_uops` load or
store uops statically; each recorded range takes one and the rest are
squashed, the way scatter/gather uops already are.  The execute part is
costed per class from the generated `perf_model/core/riscv`
in the config (see `config/riscv.cfg` for the keys and defaults): matrix
multiply-accumulates run on a matrix unit with a result latency and an
issue interval, vector dividers stay busy for their latency, and the
BOOM port assignment treats vector FP and matrix work as port 0, vector
divides as port 1 and vector memory as port 2.  `sim.stats` counts the
instructions per class under `riscv.<class>` (per thread) together with the
recorded spans and their bytes.

Two more harness cases cover this, `riscv-ime-trace` and
`riscv-ime-simulate`, built around `test/qemu-plugin-probe/rv-ime.c`, a
2x2 integer tile multiply whose rows sit in different cache lines.  They
need a qemu-riscv64 with the IME CPU properties (`x-zvvmm`, `x-zvvfmm`
for the floating-point forms, `x-zvvmtls`; `SNIPER_QEMU_RISCV_IME_CPU`
overrides the `-cpu` string) and skip otherwise.

## AME: executed by the plugin, not by QEMU

The Attached Matrix Extension (AME, Ztt, still a draft) is not in QEMU,
and the plugin does not need it to be.  Every instruction of an `ame_*`
class in the generated tables gets a second callback at translation
time, registered right after the tracing one with
`QEMU_PLUGIN_CB_RW_REGS_PC`, which executes it on a functional model of
the unit (`ame_backend.{h,cc}`: M registers, accumulators, datatype
configuration, ownership, 4x4 Int32 squares) using nothing but the v7
plugin API -- X registers through `qemu_plugin_read/write_register`,
squares through `qemu_plugin_read/write_memory_vaddr` with each row
recorded as a footprint, and `qemu_plugin_set_pc` to step over the
instruction before QEMU's illegal-instruction exception is raised.  The
X-register handles come from `qemu_plugin_get_registers`, whose names are
the gdb ABI names with x8 as `fp` (not `s0`); a register the lookup
cannot find is reported at the first AME instruction, since it would
otherwise read as zero.  The square side is the plugin option `ame_n=<N>`
(4 by default, a power of two up to 32: AME_NELEM = N*N, any power of 4
in the draft).  The
same binary dies with SIGILL without the plugin.  A semantic error in the
model (a datatype mismatch, an instruction it does not implement yet,
`mls.1r`/`mss.1r` among them) prints `[AME] error at <pc>: ...` and the
guest continues; the harness cases `riscv-ame-verify` and
`riscv-ame-simulate` (`test/riscv-gemm/gemm3.c -DUSE_AME`) fail on that
line, on any exit but 100, and on any class count or footprint byte that
differs from what `tools/riscv/expected_counts.py --ame` derives from the
square geometry.  The cost of the route is one translation-block exit per
instruction.

## The DMA-attached accelerator: tasks executed by the plugin

The DMA-attached accelerator is
not an ISA extension: the core hands it a task descriptor in memory and
waits for the result.  Sniper's `Xsnacc` overlay
(`tools/riscv/overlay/spec/std/isa/inst/Xsnacc`) defines the two hints
that carry this, `acc.task xs1` (descriptor address) and `acc.wait`, on
the CUSTOM-3 opcode, and the plugin executes them exactly as it executes
AME: `acc_backend.{h,cc}` reads the descriptor (`{X, W, bias, Y, T, nin,
nout}`), computes `Y = X W^T + bias` on the host, records the descriptor
and every input block as one contiguous footprint of `acc.task`, and at
`acc.wait` writes `Y` to guest memory and records it as the wait's
footprint.  In the simulation those footprints are what the accelerator's
DMA streams through its port (`ame_tile.cc`); the core retires `acc.task`
at once and stalls only at `acc.wait`, for whatever remains of the
transfer-compute-transfer timeline.  `test/llm-agent/agent.c -DUSE_DMA`
issues every weight product as a task; `riscv-llm-couplings` runs it.

## Emulating an extension: the plugin-API additions

The AME and Xsnacc instructions are executed by models inside this plugin,
because QEMU does not know them.  On a stock QEMU that costs an
illegal-instruction exception per instruction: the translator ends the
block with the exception, the plugin's callback runs first and moves the
PC past the instruction with `qemu_plugin_set_pc`, which exits the CPU
loop before the exception fires, and execution resumes with a block
lookup.  `tools/riscv/qemu-11.1.1-ime.patch` adds four calls to QEMU's
plugin API that remove the exception path and give the emulated unit the
rest of what an architectural extension has:

| call | what it does |
|---|---|
| `qemu_plugin_register_vcpu_insn_claim_cb` | a translation-time claim: the RISC-V translator offers every instruction it cannot decode to the plugins before it emits the exception; a plugin that recognises one registers its execution callback on it and returns true, the target emits nothing, and the block continues at the next instruction |
| `qemu_plugin_raise_exception(vaddr, cause, tval)` | an architectural exception from a callback: the model's ownership and datatype faults become illegal-instruction exceptions, a square row at an unmapped address a load or store access fault; in user mode the CPU loop delivers them as SIGILL and SIGSEGV |
| `qemu_plugin_register_vcpu_registers` | plugin-defined registers in the vcpu's gdb register list: the unit's seven CSRs, the datatype registers and the M and accumulator squares (103 registers under the feature `org.riscv.ame`), readable by gdb, by `qemu_plugin_read_register` and by other plugins |
| `qemu_plugin_register_sysreg_cb` | plugin-backed CSRs: `csrr`/`csrw` of `amenlen`, `ameudsz`, `amestype`, `ameown`, `amefflags`, `amexsat` and `amestatus` reach the model instead of trapping; the read-only ranges stay read-only |

The plugin binds the four weakly (`qemu_plugin_ext.h`): loaded into a
stock QEMU (the x86 frontend) the pointers are null and it takes the older
path, loaded into the patched QEMU it claims.  `claim=0` forces the
exception path for comparison; `trace=0` runs the models without a trace
file (a functional run).  The exit summary counts both paths:
`plugin-emulated instructions: N claimed at translation, M through the
exception path`.  The user-mode QEMU runs the plugins' vcpu-init work
before a debugger attaches (`-g`), so the registers are in the target
description gdb reads.

What the claim is worth: 4 million `ame.release` instructions (the
cheapest for the model) run in 0.18 s claimed and 0.31 s through the
exception path, about 35 ns per instruction; the agent's 4.7 million AME
instructions in a functional run take 3.1 s against 3.3 s, since the
model's own callback (register reads through the gdb path, memory
through the debug path) dominates; under tracing the SIFT write per
instruction hides both (38.6 against 38.9 s).  The probe
`test/qemu-plugin-probe/rv-ame-api.c` checks every addition and the
harness case `riscv-plugin-api` runs it, the gdb protocol read included.

