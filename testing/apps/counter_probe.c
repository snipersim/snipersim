/* counter_probe: the guest's clocks and counters under the QEMU front end
 * (testing/test_qemu.py, the *-user-counters cases).
 *
 * A guest that reads its counters directly -- RISC-V's time, cycle and
 * instret CSRs, AArch64's CNTVCT_EL0, x86's TSC -- must see the simulated
 * time, as clock_gettime() does.  Under QEMU's user mode they read the host
 * (tools/qemu-11.1.1-plugin-counters.patch lets the plugin answer them).
 * This guest reads them around three 10 ms sleeps, which the simulator
 * charges to the simulated clock while the host barely sleeps, and prints
 * the differences:
 *
 *   counter_probe: ns=N time=T freq=F cycles=C instret=I loop_instret=L
 *
 * ns is clock_gettime(CLOCK_MONOTONIC)'s difference; time the timer's
 * (rdtime, CNTVCT; 0 on x86) at the rate freq (CNTFRQ; 0 where the guest is
 * not told one); cycles the cycle counter's (rdcycle, rdtsc; 0 on AArch64);
 * instret instret's over the sleeps and loop_instret over a loop of 100,000
 * iterations (RISC-V; 0 elsewhere).
 *
 * Freestanding:
 *   clang --target=riscv64-linux-gnu -march=rv64gc -mabi=lp64d -O1 \
 *   clang --target=aarch64-linux-gnu                              -O1 \
 *   gcc                                                            -O1 \
 *       -ffreestanding -nostdlib -static [-fuse-ld=lld] -Wl,-e,_start -o counter_probe counter_probe.c
 */
#include <stdint.h>

#if defined(__x86_64__)
#define SYS_write 1
#define SYS_exit_group 231
#define SYS_nanosleep 35
#define SYS_clock_gettime 228
#else
#define SYS_write 64
#define SYS_exit_group 94
#define SYS_nanosleep 101
#define SYS_clock_gettime 113
#endif

static long sys3(long n, long a, long b, long c)
{
#if defined(__x86_64__)
   long r;
   __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
   return r;
#elif defined(__aarch64__)
   register long a0 __asm__("x0") = a; register long a1 __asm__("x1") = b; register long a2 __asm__("x2") = c;
   register long a7 __asm__("x8") = n;
   __asm__ volatile("svc #0" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
   return a0;
#else
   register long a0 __asm__("a0") = a; register long a1 __asm__("a1") = b; register long a2 __asm__("a2") = c;
   register long a7 __asm__("a7") = n;
   __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
   return a0;
#endif
}

static uint64_t now_ns(void)
{
   struct { long sec, nsec; } ts;
   sys3(SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */, (long)&ts, 0);
   return (uint64_t)ts.sec * 1000000000u + (uint64_t)ts.nsec;
}

static uint64_t rd_time(void)
{
   uint64_t v = 0;
#if defined(__riscv)
   __asm__ volatile("rdtime %0" : "=r"(v));
#elif defined(__aarch64__)
   __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
#endif
   return v;
}

static uint64_t rd_freq(void)
{
   uint64_t v = 0;
#if defined(__aarch64__)
   __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
#endif
   return v;
}

static uint64_t rd_cycles(void)
{
   uint64_t v = 0;
#if defined(__riscv)
   __asm__ volatile("rdcycle %0" : "=r"(v));
#elif defined(__x86_64__)
   uint32_t lo, hi;
   __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
   v = (uint64_t)hi << 32 | lo;
#endif
   return v;
}

static uint64_t rd_instret(void)
{
   uint64_t v = 0;
#if defined(__riscv)
   __asm__ volatile("rdinstret %0" : "=r"(v));
#endif
   return v;
}

static void out(const char *s) { long n = 0; while (s[n]) n++; sys3(SYS_write, 1, (long)s, n); }
static void outnum(uint64_t v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); out(b + i); }

void _start(void)
{
   struct { long sec, nsec; } req = { 0, 10 * 1000 * 1000 }, rem = { 0, 0 };
   const uint64_t n0 = now_ns(), t0 = rd_time(), c0 = rd_cycles(), i0 = rd_instret();
   for (int i = 0; i < 3; i++)
      sys3(SYS_nanosleep, (long)&req, (long)&rem, 0);
   const uint64_t n1 = now_ns(), t1 = rd_time(), c1 = rd_cycles(), i1 = rd_instret();
   volatile uint64_t x = 0;
   const uint64_t l0 = rd_instret();
   for (int i = 0; i < 100000; i++)
      x += i;
   const uint64_t l1 = rd_instret();
   out("counter_probe: ns="); outnum(n1 - n0);
   out(" time="); outnum(t1 - t0);
   out(" freq="); outnum(rd_freq());
   out(" cycles="); outnum(c1 - c0);
   out(" instret="); outnum(i1 - i0);
   out(" loop_instret="); outnum(l1 - l0);
   out("\n");
   sys3(SYS_exit_group, x ? 0 : 1, 0, 0);
   for (;;) {}
}
