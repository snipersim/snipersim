/* timers_raw: the guest's timers in the guest's time, through raw system
 * calls (no libc: built freestanding for x86-64, RISC-V and AArch64 alike).
 *
 * The front end keeps alarm/setitimer, the POSIX timers and timerfd
 * (sift/guest_time.h GuestTimers) and fires them at their expiry in the
 * guest's time.  Signals are blocked and taken with rt_sigtimedwait, so no
 * handler is needed: an ITIMER_REAL's SIGALRM, a POSIX timer's real-time
 * signal with its value and SI_TIMER (QEMU maps the guest's real-time
 * signals onto other host ones: the plugin sends the host's), a timerfd
 * read.  testing/test_qemu.py's *-user-timers run it on the three
 * architectures, to a file and live.  Prints
 *
 *   timers_raw: <name> want=<ns> got=<ns> ret=<result>
 *
 * then "timers_raw: done ok" or "timers_raw: done bad". */
#include <stdint.h>

#if defined(__x86_64__)
enum { NR_read = 0, NR_write = 1, NR_exit_group = 231, NR_clock_gettime = 228, NR_rt_sigprocmask = 14,
       NR_rt_sigtimedwait = 128, NR_setitimer = 38, NR_timer_create = 222, NR_timer_settime = 223,
       NR_timer_delete = 226, NR_timerfd_create = 283, NR_timerfd_settime = 286 };
#else
enum { NR_read = 63, NR_write = 64, NR_exit_group = 94, NR_clock_gettime = 113, NR_rt_sigprocmask = 135,
       NR_rt_sigtimedwait = 137, NR_setitimer = 103, NR_timer_create = 107, NR_timer_settime = 110,
       NR_timer_delete = 111, NR_timerfd_create = 85, NR_timerfd_settime = 86 };
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

static uint64_t now_ns(void)
{
   struct ts t;
   sys6(NR_clock_gettime, 1, (long)&t, 0, 0, 0, 0);
   return (uint64_t)t.sec * 1000000000u + (uint64_t)t.nsec;
}

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
   const uint64_t got = now_ns() - t0;
   out("timers_raw: "); out(name);
   out(" want="); outnum((long)want);
   out(" got="); outnum((long)got);
   out(" ret="); outnum(ret);
   out("\n");
   if (ret != want_ret)
      bad = 1;
}

#define SIGALRM 14
#define SIGRTMIN_ 34     /* the kernel's first real-time signal (glibc keeps two for itself) */
#define SI_TIMER (-2)

void _start(void)
{
   /* both signals blocked: they are taken with rt_sigtimedwait */
   uint64_t set = (1ull << (SIGALRM - 1)) | (1ull << (SIGRTMIN_ - 1));
   sys6(NR_rt_sigprocmask, 0 /* SIG_BLOCK */, (long)&set, 0, 8, 0, 0);
   struct { int32_t signo, errno_, code, pad; int32_t tid, overrun; uint64_t value; uint8_t rest[96]; } si;
   struct ts limit = { 1, 0 };

   /* setitimer(ITIMER_REAL) 2 ms: SIGALRM */
   uint64_t alrm = 1ull << (SIGALRM - 1);
   t0 = now_ns();
   struct { long isec, iusec, sec, usec; } itv = { 0, 0, 0, 2000 };
   sys6(NR_setitimer, 0, (long)&itv, 0, 0, 0, 0);
   long r = sys6(NR_rt_sigtimedwait, (long)&alrm, (long)&si, (long)&limit, 8, 0, 0);
   report("itimer", 2000000, r, SIGALRM);

   /* a POSIX timer, 3 ms on CLOCK_MONOTONIC: SIGRTMIN with its value */
   uint64_t rt = 1ull << (SIGRTMIN_ - 1);
   struct { uint64_t value; int32_t signo, notify, tid, pad; } ev = { 4242, SIGRTMIN_, 0 /* SIGEV_SIGNAL */, 0, 0 };
   int32_t timer = -1;
   sys6(NR_timer_create, 1, (long)&ev, (long)&timer, 0, 0, 0);
   t0 = now_ns();
   struct ts its[2] = { { 0, 0 }, { 0, 3000000 } };
   sys6(NR_timer_settime, timer, 0, (long)its, 0, 0, 0);
   r = sys6(NR_rt_sigtimedwait, (long)&rt, (long)&si, (long)&limit, 8, 0, 0);
   report("posix-timer", 3000000, r == SIGRTMIN_ && si.code == SI_TIMER ? (long)si.value : -1, 4242);
   sys6(NR_timer_delete, timer, 0, 0, 0, 0, 0);

   /* a timerfd, 4 ms, read */
   const long fd = sys6(NR_timerfd_create, 1, 0, 0, 0, 0, 0);
   t0 = now_ns();
   struct ts tfd[2] = { { 0, 0 }, { 0, 4000000 } };
   sys6(NR_timerfd_settime, fd, 0, (long)tfd, 0, 0, 0);
   uint64_t n = 0;
   r = sys6(NR_read, fd, (long)&n, 8, 0, 0, 0);
   report("timerfd", 4000000, r == 8 ? (long)n : -1, 1);

   out(bad ? "timers_raw: done bad\n" : "timers_raw: done ok\n");
   sys6(NR_exit_group, bad, 0, 0, 0, 0, 0);
   for (;;) {}
}
