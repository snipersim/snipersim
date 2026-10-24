/* phased_st.c for a guest without a C library: the RISC-V toolchain here is a
 * clang with no sysroot, so the chase array is static, output goes through
 * the write system call and exit through exit_group (RISC-V, or AArch64 built
 * with --target=aarch64-linux-gnu: both use the generic call numbers), and the checksum is an
 * integer.  One thread, phases alternating a pointer chase with arithmetic,
 * SimRoiStart/SimRoiEnd around the phases, the checksum printed inside the
 * ROI.  Build:
 *   $SNIPER_RISCV_CC --target=riscv64-linux-gnu -march=rv64gc -mabi=lp64d -O1 \
 *       -ffreestanding -nostdlib -static -fuse-ld=lld -Wl,-e,_start -I include \
 *       -o phased_rv testing/apps/phased_rv.c */
#include <stdint.h>
#include "sim_api.h"

#ifndef NPHASES
#define NPHASES 12
#endif
#ifndef CHASE_SLOTS
#define CHASE_SLOTS (1 << 17)
#endif
#ifndef CHASE_STEPS
#define CHASE_STEPS 60000
#endif
#ifndef COMPUTE_ITERS
#define COMPUTE_ITERS 120000
#endif

static uint32_t chase[CHASE_SLOTS];

static long sys3(long n, long a, long b, long c)
{
#if defined(__aarch64__)
   /* the same Linux call numbers (the generic table) on AArch64, through svc */
   register long a0 __asm__("x0") = a; register long a1 __asm__("x1") = b; register long a2 __asm__("x2") = c;
   register long a7 __asm__("x8") = n;
   __asm__ volatile("svc #0" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
#else
   register long a0 __asm__("a0") = a; register long a1 __asm__("a1") = b; register long a2 __asm__("a2") = c;
   register long a7 __asm__("a7") = n;
   __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
#endif
   return a0;
}
static void out(const char *s) { long n = 0; while (s[n]) n++; sys3(64, 1, (long)s, n); }
static void outnum(unsigned long v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); out(b + i); }
static uint64_t lcg(uint64_t *s) { *s = *s * 6364136223846793005ull + 1442695040888963407ull; return *s >> 33; }

void _start(void)
{
   uint64_t seed = 12345;
   for (uint32_t i = 0; i < CHASE_SLOTS; i++) chase[i] = i;
   for (uint32_t i = CHASE_SLOTS - 1; i > 0; i--) { uint32_t j = (uint32_t)(lcg(&seed) % (i + 1)); uint32_t t = chase[i]; chase[i] = chase[j]; chase[j] = t; }
   uint64_t acc = 0; unsigned long seen = 0; uint32_t p = 0;

   SimRoiStart();
   for (int phase = 0; phase < NPHASES; phase++)
   {
      if (phase & 1)
         for (long i = 0; i < COMPUTE_ITERS; i++) acc = acc * 6364136223846793005ull + (uint64_t)(i & 7);
      else
         for (long i = 0; i < CHASE_STEPS; i++) { p = chase[p]; seen++; }
   }
   out("phased_rv: phases="); outnum(NPHASES); out(" checksum="); outnum(acc); out(" visited="); outnum(seen); out(" p="); outnum(p); out("\n");
   SimRoiEnd();
   sys3(94, 0, 0, 0);   /* exit_group */
   for (;;) {}
}
