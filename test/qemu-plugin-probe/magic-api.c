/* Every standard magic command of sim_api.h, from a freestanding guest that
 * builds for x86-64, AArch64 and RISC-V (testing/test_magic.py).
 *
 * Each command's answer is printed as "api <name>=<value>", so that the test
 * can hold each one against what the simulator was set up to say: the core
 * and thread ids, the core count, a frequency set and read back, a user
 * command a script answers, the instruction count and simulated time moving
 * on across some work.  Markers and the thread name have their effect on the
 * simulator side, where the test looks for them.  Without a simulator every
 * command comes back as itself, as on hardware.  Raw Linux system calls. */
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
static void show(const char *name, u64 v) { put("api "); put(name); put("="); putn(v); put("\n"); }

static unsigned buf[4096];
static u64 work(int n)
{
   u64 sum = 0;
   for (int r = 0; r < n; r++)
      for (int i = 0; i < 4096; i++)
      {
         buf[i] = buf[i] * 3 + (unsigned)i;
         sum += (buf[i] & 1) ? buf[i] >> 3 : buf[i] & 7;
      }
   return sum;
}

/* Strings the simulator reads out of guest memory (up to 256 bytes) */
static const char thread_name[256] = "api-worker";
static const char marker_name[256] = "api-marker";

void _start(void)
{
   show("in_simulator", SimInSimulator() ? 1 : 0);
   show("proc_id", SimGetProcId());
   show("thread_id", SimGetThreadId());
   show("num_procs", SimGetNumProcs());
   show("num_threads", SimGetNumThreads());
   SimSetThreadName(thread_name);
   SimRoiStart();
   u64 mhz = SimGetFreqMHz(0);
   show("mhz_initial", mhz);
   SimSetFreqMHz(0, 1234);
   show("mhz_after_set", SimGetFreqMHz(0));
   show("user", SimUser(0x5a5a, 41));
   SimMarker(7, 8);
   SimNamedMarker(9, marker_name);
   u64 i0 = SimMagic1(SIM_CMD_GET_INS_NUM, 0), t0 = SimMagic0(SIM_CMD_GET_SIM_TIME);
   u64 s = work(4);
   u64 i1 = SimMagic1(SIM_CMD_GET_INS_NUM, 0), t1 = SimMagic0(SIM_CMD_GET_SIM_TIME);
   show("ins_delta", i1 - i0);
   show("time_grew", t1 > t0 ? 1 : 0);
   SimRoiEnd();
   show("work", s);
   put("api done\n");
   sys3(SYS_EXIT, 0, 0, 0);
   for (;;) ;
}
