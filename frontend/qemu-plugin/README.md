# QEMU frontend

A Sniper frontend that loads into a **stock** QEMU as a TCG plugin. No QEMU
fork, no QEMU patches. It writes SIFT, so to Sniper it is another trace
producer.

```
make QEMU=ON -j8          # builds QEMU with plugins, then lib/libsniper-qemu.so
```

## Running

User mode, single-threaded guest:

```
qemu-x86_64 -plugin build/default/lib/libsniper-qemu.so,output=trace -- ./app
./run-sniper -n 1 -c nehalem-lite --traces=trace.app0.th0.sift
```

Full system, with Sniper owning guest time:

```
qemu-system-x86_64 -accel tcg -plugin \
  build/default/lib/libsniper-qemu.so,output=trace,time,ns=1 ...
```

Options: `output=` prefix, `flow=N` instructions per quantum, `response`
(user-mode blocking flow control), `time` (system-mode time control), `ns=N`
nanoseconds per instruction, `verbose`.

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
regeneration (RISCV_ISA_GENERATION.md).

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
(`tools/riscv/gen_isa.py`, see RISCV_ISA_GENERATION.md): the same
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
`Sift::MAX_MEMORY_SPANS` = 16 per instruction; more are dropped with a
warning) and the reader delivers them with it (`Instruction::spans`).
`siftdump` prints them as `-- spans <addr>+<bytes><r|w>`.  A trace from
the Pin recorder never contains the record, and a reader that predates it
skips it.

**Simulation.**  The trace thread turns each range into one memory access
of that size (the core splits it over cache lines), so every line a tile
touches goes through the cache hierarchy and the ROB.  A vector or tile
memory instruction is given `perf_model/core/rvv/memory_uops` load or
store uops statically; each recorded range takes one and the rest are
squashed, the way scatter/gather uops already are.  The execute part is
costed per class from `perf_model/core/rvv` and `perf_model/core/ime`
in the config (see `config/riscv.cfg` for the keys and defaults): matrix
multiply-accumulates run on a matrix unit with a result latency and an
issue interval, vector dividers stay busy for their latency, and the
BOOM port assignment treats vector FP and matrix work as port 0, vector
divides as port 1 and vector memory as port 2.  `sim.stats` counts the
instructions per class under `rvext.*` (per thread) together with the
recorded spans and their bytes.

Two more harness cases cover this, `riscv-ime-trace` and
`riscv-ime-simulate`, built around `test/qemu-plugin-probe/rv-ime.c`, a
2x2 integer tile multiply whose rows sit in different cache lines.  They
need a qemu-riscv64 with the IME CPU properties (`x-zvvmm`,
`x-zvvmtls`; `SNIPER_QEMU_RISCV_IME_CPU` overrides the `-cpu` string)
and skip otherwise.

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
same binary dies with SIGILL without the plugin.  A semantic error in the
model (a datatype mismatch, an instruction it does not implement yet,
`mls.1r`/`mss.1r` among them) prints `[AME] error at <pc>: ...` and the
guest continues; the harness cases `riscv-ame-verify` and
`riscv-ame-simulate` (`test/riscv-gemm/gemm3.c -DUSE_AME`) fail on that
line, on any exit but 100, and on any class count or footprint byte that
differs from what `tools/riscv/expected_counts.py --ame` derives from the
square geometry.  The cost of the route is one translation-block exit per
instruction.
