/* A freestanding guest with a region of interest, for x86-64, AArch64 and
 * RISC-V: five rounds of work before SimRoiStart(), ten inside, five after,
 * so the ROI holds about half the instructions.  It reports whether it ran
 * under a simulator that answers magic instructions (SimInSimulator()) and
 * its checksum, and exits with the checksum's low bits.  Raw Linux system
 * calls, no libc. */
#include "sim_api.h"

typedef unsigned long u64;

static long sys3(long n, long a, long b, long c)
{
#if defined(__x86_64__)
   long ret;
   register long rdi asm("rdi") = a, rsi asm("rsi") = b, rdx asm("rdx") = c;
   asm volatile("syscall" : "=a"(ret) : "a"(n), "r"(rdi), "r"(rsi), "r"(rdx) : "rcx", "r11", "memory");
   return ret;
#elif defined(__aarch64__)
   register long x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c, x8 asm("x8") = n;
   asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
   return x0;
#elif defined(__riscv)
   register long a0 asm("a0") = a, a1 asm("a1") = b, a2 asm("a2") = c, a7 asm("a7") = n;
   asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
   return a0;
#endif
}
#if defined(__x86_64__)
#define SYS_WRITE 1
#define SYS_EXIT  60
#else
#define SYS_WRITE 64
#define SYS_EXIT  93
#endif

static void put(const char *s) { long n = 0; while (s[n]) n++; sys3(SYS_WRITE, 1, (long)s, n); }
static void putn(u64 v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); put(b + i); }

static unsigned buf[4096];

static u64 work(int n)
{
   u64 sum = 0;
   for (int r = 0; r < n; r++)
      for (int i = 0; i < 4096; i++)
      {
         buf[i] = buf[i] * 3 + (unsigned)i;
         if (buf[i] & 1)
            sum += buf[i] >> 3;
         else
            sum ^= buf[i];
      }
   return sum;
}

void _start(void)
{
   u64 s = work(5);
   SimRoiStart();
   s += work(10);
   SimRoiEnd();
   s += work(5);
   put("roi in_sim="); putn(SimInSimulator() ? 1 : 0); put(" sum="); putn(s); put("\n");
   sys3(SYS_EXIT, (long)(s & 0x7f), 0, 0);
   for (;;) ;
}
