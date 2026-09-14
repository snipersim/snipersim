# QEMU plugin probe

Not a frontend. This is the spike that settled how a QEMU-based Sniper
frontend has to be built, kept because the questions it answers will come up
again every time QEMU's plugin API moves.

Build it against a QEMU source tree and run the checks below.

```
make QEMU_SRC=/path/to/qemu
```

## What it establishes

**Instruction counting is exact.** `boot.S` is a boot sector whose executed
instruction count can be computed by hand: a 7-instruction prologue, then
`OUTER` iterations of (1 + 3*`INNER` + 2) instructions, then one `hlt` that
never wakes because interrupts are disabled. At 1000x1000 that is 3,003,008.
The probe counts only `[0x7c00, 0x7e00)`, so firmware is excluded.

```
qemu-system-x86_64 -nographic -accel tcg -m 64 \
  -drive file=boot.img,format=raw,if=floppy \
  -plugin ./probe.so,time,quantum=100000,lo=0x7c00,hi=0x7e00 -no-reboot
```

Measured 3,003,008 — exact, and unchanged across repeated runs and under
`-smp 4`. Cross-checked against the DynamoRIO frontend on the same user-mode
binary: 32,057,806 (DR) vs 32,049,836 (QEMU), 0.025% apart.

**Paged VA -> PA translation is correct.** `boot64.S` enters long mode with
4-level paging and maps VA `0x40000000` to PA `0x200000` -- deliberately not an
identity mapping, so no shortcut can produce the right answer.

```
qemu-system-x86_64 -nographic -accel tcg -m 128 \
  -drive file=boot64.img,format=raw,if=floppy \
  -plugin ./probe.so,watch=0x40000000 -no-reboot
```

```
PROBE WATCH va=0x40000000 hwaddr_pa=0x200000 translate_vaddr=0x200000 store
PROBE WATCH va=0x40000000 hwaddr_pa=0x200000 translate_vaddr=0x200000 load
```

Both `qemu_plugin_hwaddr_phys_addr()` and `qemu_plugin_translate_vaddr()` --
independent paths -- agree with the page tables the guest built.

**Time control works.** `qemu_plugin_request_time_control()` is granted in
system mode and `qemu_plugin_update_ns()` drives guest time. This is what lets
Sniper own simulated time without holding a vCPU.

**Blocking in a callback is safe only when nothing waits on another vCPU.**
`mode=block` sleeps inside an instruction callback:

- single-threaded (`st`): completes, instruction count unchanged.
- `mtfork` with `block=8000,blockfrom=1`: `fork` is observed to *wait* for the
  blocked vCPUs before completing. `start_exclusive()` (`cpu-common.c`) counts
  every CPU with `running == true`, and a vCPU inside a plugin callback is one
  of them.
- `mtfork` with `barrier`: models Sniper's cross-thread flow control, where a
  vCPU may not continue until a peer advances. This **deadlocks**, and the probe
  detects it and exits 42.

`pthread_create` is *not* a trigger: in `linux-user/syscall.c`, `CLONE_VM`
takes the host-`pthread_create` path and never calls `fork_start()`. Real
`fork()` does. Code-cache flushes are also exclusive operations and were
observed in runs as small as 7.7M instructions, so this is not avoidable by
avoiding `fork()`.

Consequence: user mode is safe single-threaded only; full system uses time
control instead of blocking and is not exposed to this.

## Options

```
quantum=N     trap every N instructions (inline counter + conditional callback)
block=MS      sleep MS inside the callback
blockfrom=N   only block vCPUs with index >= N
barrier       block until a peer vCPU advances (models Sniper flow control)
time          request time control and drive guest time (system mode)
lo=,hi=       count only instructions in [lo, hi)
watch=VA      report the physical address a virtual address resolves to
```

## API versions

`compat.h` spans plugin API v3 (QEMU 9.1) and v7 (QEMU 11.1). v7 replaced the
leading `qemu_plugin_id_t` on callbacks with a trailing `void *userdata`, and
the header moved from `include/qemu/` to `include/plugins/`. Everything else
we use is unchanged.

The versions are not interchangeable at run time, which is why the build pins
one and compiles the plugin against it:

```
plugin requires API version 3, but this QEMU supports only a minimum version of 7
plugin requires API version 7, but this QEMU supports only up to version 3
```
