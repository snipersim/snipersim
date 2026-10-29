/* timed_raw: the guest's timeouts in the guest's time, through raw system
 * calls (no libc: built freestanding for x86-64, RISC-V and AArch64 alike).
 *
 * No time the guest gives reaches the host's kernel (sift/guest_time.h).
 * Each timed wait below has nothing to wait for but its timeout, and ends
 * at its deadline in the guest's time -- the simulator's, or the one-IPC
 * model's where no simulator answers -- measured with clock_gettime, the
 * guest's clock.  The 2 s one must not take 2 s of the host's.  The time
 * pselect6 leaves in its timespec is checked too (the kernel writes it).
 * testing/test_qemu.py timed-raw runs it on the three architectures, to a
 * file and live.  Prints one line per wait,
 *
 *   timed_raw: <name> want=<ns> got=<ns> ret=<result>
 *
 * then "timed_raw: done ok" or "timed_raw: done bad". */
#include <stdint.h>

#if defined(__x86_64__)
enum { NR_write = 1, NR_exit_group = 231, NR_clock_gettime = 228, NR_clock_nanosleep = 230, NR_ppoll = 271,
       NR_pselect6 = 270, NR_epoll_create1 = 291, NR_epoll_pwait = 281, NR_futex = 202, NR_rt_sigtimedwait = 128 };
#else
enum { NR_write = 64, NR_exit_group = 94, NR_clock_gettime = 113, NR_clock_nanosleep = 115, NR_ppoll = 73,
       NR_pselect6 = 72, NR_epoll_create1 = 20, NR_epoll_pwait = 22, NR_futex = 98, NR_rt_sigtimedwait = 137 };
#endif

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
#if defined(__x86_64__)
   long r;
   register long r10 __asm__("r10") = d; register long r8 __asm__("r8") = e; register long r9 __asm__("r9") = f;
   __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
   return r;
#elif defined(__aarch64__)
   register long x0 __asm__("x0") = a; register long x1 __asm__("x1") = b; register long x2 __asm__("x2") = c;
   register long x3 __asm__("x3") = d; register long x4 __asm__("x4") = e; register long x5 __asm__("x5") = f;
   register long x8 __asm__("x8") = n;
   __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
   return x0;
#else
   register long a0 __asm__("a0") = a; register long a1 __asm__("a1") = b; register long a2 __asm__("a2") = c;
   register long a3 __asm__("a3") = d; register long a4 __asm__("a4") = e; register long a5 __asm__("a5") = f;
   register long a7 __asm__("a7") = n;
   __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7) : "memory");
   return a0;
#endif
}

struct ts { long sec, nsec; };

static uint64_t clock_ns(int clock)
{
   struct ts t;
   sys6(NR_clock_gettime, clock, (long)&t, 0, 0, 0, 0);
   return (uint64_t)t.sec * 1000000000u + (uint64_t)t.nsec;
}
static struct ts rel(uint64_t ns) { struct ts t = { (long)(ns / 1000000000u), (long)(ns % 1000000000u) }; return t; }

static void out(const char *s) { long n = 0; while (s[n]) n++; sys6(NR_write, 1, (long)s, n, 0, 0, 0); }
static void outnum(long v)
{
   char b[24];
   int i = 23, neg = v < 0;
   unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
   b[i] = 0;
   do { b[--i] = '0' + u % 10; u /= 10; } while (u);
   if (neg)
      b[--i] = '-';
   out(b + i);
}

static int bad = 0;
static uint64_t t0;
static void report(const char *name, uint64_t want, long ret, long want_ret)
{
   const uint64_t got = clock_ns(1) - t0;
   out("timed_raw: "); out(name);
   out(" want="); outnum((long)want);
   out(" got="); outnum((long)got);
   out(" ret="); outnum(ret);
   out("\n");
   if (ret != want_ret)
      bad = 1;
}

void _start(void)
{
   struct ts t;
   long r;

   t0 = clock_ns(1);
   t = rel(clock_ns(1) + 3000000);
   r = sys6(NR_clock_nanosleep, 1 /* CLOCK_MONOTONIC */, 1 /* TIMER_ABSTIME */, (long)&t, 0, 0, 0);
   report("clock_nanosleep-abs", 3000000, r, 0);

   t0 = clock_ns(1);
   t = rel(2000000);
   r = sys6(NR_ppoll, 0, 0, (long)&t, 0, 8, 0);
   report("ppoll", 2000000, r, 0);

   t0 = clock_ns(1);
   t = rel(3000000);
   r = sys6(NR_pselect6, 0, 0, 0, 0, (long)&t, 0);
   report("pselect6", 3000000, r, 0);
   if (t.sec || t.nsec)
   {
      out("timed_raw: pselect6 left time in its timespec\n");
      bad = 1;
   }

   const long ep = sys6(NR_epoll_create1, 0, 0, 0, 0, 0, 0);
   uint64_t ev[2];
   t0 = clock_ns(1);
   r = sys6(NR_epoll_pwait, ep, (long)ev, 1, 4, 0, 8);
   report("epoll_pwait", 4000000, r, 0);

   volatile int word = 0;
   t0 = clock_ns(1);
   t = rel(5000000);
   r = sys6(NR_futex, (long)&word, 0 | 128 /* FUTEX_WAIT_PRIVATE */, 0, (long)&t, 0, 0);
   report("futex-wait", 5000000, r, -110);

   t0 = clock_ns(1);
   t = rel(clock_ns(0) + 2000000);
   r = sys6(NR_futex, (long)&word, 9 | 128 | 256 /* FUTEX_WAIT_BITSET_PRIVATE|FUTEX_CLOCK_REALTIME */, 0, (long)&t, 0, -1);
   report("futex-bitset-realtime", 2000000, r, -110);

   uint64_t set = 1ull << (12 - 1);   /* SIGUSR2, not blocked: never arrives */
   t0 = clock_ns(1);
   t = rel(3000000);
   r = sys6(NR_rt_sigtimedwait, (long)&set, 0, (long)&t, 8, 0, 0);
   report("rt_sigtimedwait", 3000000, r, -11);

   t0 = clock_ns(1);
   t = rel(2000000000);
   r = sys6(NR_ppoll, 0, 0, (long)&t, 0, 8, 0);
   report("ppoll-2s", 2000000000, r, 0);

   out(bad ? "timed_raw: done bad\n" : "timed_raw: done ok\n");
   sys6(NR_exit_group, bad, 0, 0, 0, 0, 0);
   for (;;) {}
}
