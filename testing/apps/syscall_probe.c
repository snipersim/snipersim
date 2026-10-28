/* syscall_probe: a guest for the system-call checks under the QEMU front end
 * (testing/test_qemu.py, riscv-user-syscalls and aarch64-user-syscalls).
 *
 * The simulator models a handful of system calls by their x86-64 numbers; a
 * RISC-V or AArch64 guest uses the asm-generic table, where the same numbers
 * mean other calls.  Before the plugin translated them, unlinkat (35 on
 * asm-generic, nanosleep on x86-64) had 16 zero bytes written over its
 * pathname, and mlock (228, clock_gettime on x86-64) aborted the simulator.
 * This guest makes those calls and checks what the simulator did with them:
 *
 *   - nanosleep for 3 x 10 ms: the simulated time must have advanced by at
 *     least 30 ms (the emulation charges the sleep to the core);
 *   - unlinkat on a pathname that does not exist: the pathname's bytes must
 *     be intact afterwards (exit 101 if not);
 *   - mlock on a page of its own, munlockall, dup3: the simulator must still
 *     be running afterwards (it printed "syscall_probe: done" and exits 100);
 *   - sched_getaffinity: the simulator emulates it, and its answer -- the
 *     simulated cores, not the host's -- must reach the guest; the probe
 *     prints "syscall_probe: cpus=N".
 *   - clock_gettime (CLOCK_MONOTONIC) and gettimeofday around the sleeps:
 *     the simulator answers them with simulated time, so they see the 30 ms
 *     the sleeps were charged, where the host clock sees the moment the
 *     emulated sleeps took; and clock_gettime on CLOCK_PROCESS_CPUTIME_ID
 *     succeeds (it used to abort the simulator).  The probe prints
 *     "syscall_probe: slept_us=N tod_us=N cputime=R".
 *
 * Freestanding:
 *   clang --target=riscv64-linux-gnu -march=rv64gc -mabi=lp64d -O1 \
 *   clang --target=aarch64-linux-gnu                              -O1 \
 *       -ffreestanding -nostdlib -static -fuse-ld=lld -Wl,-e,_start -o syscall_probe syscall_probe.c
 */
#include <stdint.h>

static long sys4(long n, long a, long b, long c, long d)
{
#if defined(__aarch64__)
   register long a0 __asm__("x0") = a; register long a1 __asm__("x1") = b; register long a2 __asm__("x2") = c; register long a3 __asm__("x3") = d;
   register long a7 __asm__("x8") = n;
   __asm__ volatile("svc #0" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
#else
   register long a0 __asm__("a0") = a; register long a1 __asm__("a1") = b; register long a2 __asm__("a2") = c; register long a3 __asm__("a3") = d;
   register long a7 __asm__("a7") = n;
   __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
#endif
   return a0;
}

/* asm-generic numbers (RISC-V and AArch64 share them) */
#define SYS_dup3        24
#define SYS_unlinkat    35
#define SYS_write       64
#define SYS_exit_group  94
#define SYS_nanosleep   101
#define SYS_sched_getaffinity 123
#define SYS_clock_gettime 113
#define SYS_gettimeofday 169
#define SYS_mlock       228
#define SYS_munlockall  231

static volatile unsigned char page[8192] __attribute__((aligned(4096)));

void _start(void)
{
   struct { long sec, nsec; } req = { 0, 10 * 1000 * 1000 }, rem = { 0, 0 };
   struct { long sec, nsec; } t0, t1, cpu;
   struct { long sec, usec; } tv0, tv1;
   sys4(SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */, (long)&t0, 0, 0);
   sys4(SYS_gettimeofday, (long)&tv0, 0, 0, 0);
   for (int i = 0; i < 3; i++)
      sys4(SYS_nanosleep, (long)&req, (long)&rem, 0, 0);
   sys4(SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */, (long)&t1, 0, 0);
   sys4(SYS_gettimeofday, (long)&tv1, 0, 0, 0);
   long cpu_rc = sys4(SYS_clock_gettime, 2 /* CLOCK_PROCESS_CPUTIME_ID */, (long)&cpu, 0, 0);
   long slept = (t1.sec - t0.sec) * 1000000 + (t1.nsec - t0.nsec) / 1000;
   long tod = (tv1.sec - tv0.sec) * 1000000 + (tv1.usec - tv0.usec);
   char tline[] = "syscall_probe: slept_us=0000000 tod_us=0000000 cputime=0\n";
   for (int k = 0; k < 7; k++) { long d = 1; for (int e = 0; e < 6 - k; e++) d *= 10; tline[24 + k] = '0' + (slept < 0 ? 0 : slept / d % 10); tline[39 + k] = '0' + (tod < 0 ? 0 : tod / d % 10); }
   tline[55] = cpu_rc == 0 ? '0' : '1';
   sys4(SYS_write, 1, (long)tline, sizeof(tline) - 1, 0);

   static volatile char path[] = "/nonexistent-sniper-syscall-probe";
   static const char expect[] = "/nonexistent-sniper-syscall-probe";
   sys4(SYS_unlinkat, -100 /* AT_FDCWD */, (long)path, 0, 0);
   int intact = 1;
   for (unsigned i = 0; i < sizeof(expect); i++)
      if (path[i] != expect[i]) intact = 0;

   page[0] = 1;
   sys4(SYS_mlock, (long)page, 4096, 0, 0);
   sys4(SYS_munlockall, 0, 0, 0, 0);
   sys4(SYS_dup3, 1, 7, 0, 0);

   /* The kernel (and the simulator) return the bytes of mask they filled:
      one byte for up to eight CPUs. */
   static unsigned char mask[128];
   long got = sys4(SYS_sched_getaffinity, 0, sizeof(mask), (long)mask, 0);
   int cpus = 0;
   for (long i = 0; got > 0 && i < got && i < (long)sizeof(mask); i++)
      for (unsigned m = mask[i]; m; m &= m - 1)
         cpus++;
   char line[] = "syscall_probe: cpus=000\n";
   line[20] = '0' + cpus / 100 % 10; line[21] = '0' + cpus / 10 % 10; line[22] = '0' + cpus % 10;
   sys4(SYS_write, 1, (long)line, sizeof(line) - 1, 0);

   static const char msg[] = "syscall_probe: done\n";
   sys4(SYS_write, 1, (long)msg, sizeof(msg) - 1, 0);
   sys4(SYS_exit_group, intact ? 100 : 101, 0, 0, 0);
   for (;;) {}
}
