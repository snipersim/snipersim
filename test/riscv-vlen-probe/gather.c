/* A footprint the trace record cannot hold: NGATHER indexed loads of a whole
 * e32 m8 group, every element from its own cache line with a line skipped
 * between them (adjacent lines would coalesce into one span, rightly).  The
 * harness derives the lines per gather from VLEN and checks
 * riscv.spans_dropped_lines against them. */
#include <stdint.h>
#ifndef NGATHER
#define NGATHER 8
#endif
static void sys_exit(long code) { register long a0 __asm__("a0") = code; register long a7 __asm__("a7") = 93; __asm__ volatile("ecall" : : "r"(a0), "r"(a7)); for (;;) ; }
static uint32_t table[16384 * 32] __attribute__((aligned(64)));      /* 16384 elements, every other line */
static uint32_t idx[16384] __attribute__((aligned(64)));
static uint32_t out[16384] __attribute__((aligned(64)));
void _start(void)
{
   long vb; __asm__ volatile("csrr %0, vlenb" : "=r"(vb));
   const long n = vb * 8 / 4;                        /* e32 m8 lanes */
   for (long i = 0; i < n; i++) { idx[i] = (uint32_t)(i * 128); table[i * 32] = (uint32_t)(i * 3 + 1); }
   for (int g = 0; g < NGATHER; g++)
   {
      long vl;
      __asm__ volatile("vsetvli %0, %1, e32, m8, ta, ma\n\tvle32.v v8, (%2)\n\tvluxei32.v v16, (%3), v8\n\tvse32.v v16, (%4)" : "=&r"(vl) : "r"(n), "r"(idx), "r"(table), "r"(out) : "memory");
      for (long i = 0; i < n; i++) if (out[i] != (uint32_t)(i * 3 + 1)) sys_exit(10 + g);
   }
   sys_exit(100);
}
