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

## Known limitation

A branch is taken to be the last instruction of a translation block. That is
right for the common case and wrong when a block ends for another reason. It
affects branch-predictor input, not the instruction stream; the fix is to
decode with XED (x86) or Capstone (ARM), both already dependencies.
