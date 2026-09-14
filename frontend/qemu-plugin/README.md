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
