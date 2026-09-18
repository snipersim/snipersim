# emu-probe: can a plugin alone emulate an instruction QEMU does not know?

Yes, on QEMU 11.1.1 (plugin API v7), with no QEMU change.  `emu_probe.c`
claims the AME word `0xa4a0002b` (`ame.release`, a CUSTOM-1 encoding QEMU
traps on) at translation, and at execution -- registered with
`QEMU_PLUGIN_CB_RW_REGS_PC` -- reads `a1`, writes `a0 = a1 + 35`
(`qemu_plugin_write_register`), stores a marker through
`qemu_plugin_write_memory_vaddr`, and moves the PC past the instruction
with `qemu_plugin_set_pc`, which exits the CPU loop before the
illegal-instruction code that QEMU translated for the word ever runs.
`guest.c` executes the word with `a1 = 7` and exits with `a0` plus 100 if
the marker landed:

    clang --target=riscv64-linux-gnu -march=rv64gc -mabi=lp64d -O1 -ffreestanding \
          -nostdlib -static -fuse-ld=lld -Wl,-e,_start -o guest guest.c
    gcc -O2 -fPIC -shared -I <qemu>/include/plugins $(pkg-config --cflags glib-2.0) \
        -o emu_probe.so emu_probe.c
    qemu-riscv64 guest                         # SIGILL (exit 132)
    qemu-riscv64 -plugin ./emu_probe.so guest  # exit 142

What this rests on, in the QEMU source: `qemu_plugin_write_register` goes
through `gdb_write_register` and is allowed under `RW_REGS`/`RW_REGS_PC`,
whose helper flags are 0 so TCG spills and reloads the globals around the
callback (a write persists); `qemu_plugin_set_pc` asserts `RW_REGS_PC`,
sets the PC and calls `cpu_loop_exit`.  Registers a plugin can reach are
those the target's gdbstub exposes: for RISC-V the integer registers and
PC, the FP registers, every CSR of the configured privilege version
(`riscv-csr.xml`) and the vector registers when V is enabled
(`riscv-vector.xml`).

Two consequences for the frontend.  The emulation must live in the same
plugin as the tracer (or be registered after it): `cpu_loop_exit` inside a
callback abandons the remaining callbacks queued for that instruction.
And each emulated instruction costs a translation-block exit and re-entry,
fine for functional work; a translation-time "claim" API in QEMU (skip
`translate_insn` for a claimed instruction and advance the PC inline)
would remove both the exit and the dead illegal-instruction code.
