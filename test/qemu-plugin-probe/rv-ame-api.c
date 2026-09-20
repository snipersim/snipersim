/* A guest for the plugin-API additions that let the QEMU frontend emulate an
 * extension it does not know (see frontend/qemu-plugin/README.md): the AME
 * CSRs the plugin backs (csrr/csrw reach the model instead of trapping), the
 * claimed instructions (the block runs through them), and the exceptions
 * the model raises.  Freestanding, entered at _start.
 *
 *   -DMODE=0  the CSRs, ownership, one 4x4 product through claimed
 *             instructions: exits 100 when everything checks
 *   -DMODE=1  an AME instruction without owning the backend: the draft says
 *             illegal instruction, so the process dies with SIGILL
 *   -DMODE=2  a square load from an unmapped address: a load access fault,
 *             so the process dies with SIGSEGV
 *
 * Other exit codes say which check failed (3..12). */
#include "riscv_insn.h"
#ifndef MODE
#define MODE 0
#endif
#define CSRR(v, n) __asm__ volatile("csrr %0, %1" : "=r"(v) : "i"(n))
#define CSRW(n, v) __asm__ volatile("csrw %0, %1" : : "i"(n), "r"(v))
#define AMENLEN 0xCC0
#define AMEUDSZ 0xCC1
#define AMEOWN 0xCC3
#define AMESTATUS 0x800

typedef int i32;
static i32 A[16] __attribute__((aligned(64))), B[16] __attribute__((aligned(64))), C[16] __attribute__((aligned(64)));
char stack_area[8192] __attribute__((aligned(16)));
__asm__(".globl _start\n_start:\n\tlla sp, stack_area + 8192\n\tcall main\n\tli a7, 93\n\tecall\n");

int main(void)
{
   unsigned long v;
   long status;
   CSRR(v, AMENLEN);   if (v != 4) return 3;      /* the plugin's square side (ame_n, 4 by default) */
   CSRR(v, AMEUDSZ);   if (v != 32) return 4;     /* 32-bit elements */
   CSRR(v, AMEOWN);    if (v != 0) return 5;      /* nothing owned yet */
   CSRW(AMESTATUS, 0x55UL);
   CSRR(v, AMESTATUS); if (v != 0x55) return 6;   /* a writable one, round trip */
#if MODE == 1
   RISCV_ASETTYP(0, 0x40000020L);                 /* without ownership: illegal instruction */
   return 7;
#endif
   RISCV_AME_ACQUIRE(status, 0L);
   if (status != 1) return 8;
   CSRR(v, AMEOWN);    if (v != 1) return 9;
   RISCV_MSETTYP(0, 0x40000020L);
   RISCV_MSETTYP(1, 0x40000020L);
   RISCV_MSETTYP(2, 0x40000020L);
   RISCV_ASETTYP(0, 0x40000020L);
#if MODE == 2
   RISCV_MLS_ST(0, 0x10L, 16L);                   /* unmapped: load access fault */
   return 10;
#endif
   for (int i = 0; i < 16; i++) { A[i] = (i / 4 == i % 4) ? 1 : 0; B[i] = i; C[i] = -1; }
   RISCV_MLS_ST(0, (long)A, 16L);                 /* M0 = A (identity) */
   RISCV_MLS_ST(1, (long)B, 16L);                 /* M1 = B */
   RISCV_MMULBTACC_2D(0, 0, 1);                   /* acc0 += A x B^T */
   RISCV_MMOV_M_A(2, 0);
   RISCV_MSS_ST(2, (long)C, 16L);                 /* C = B^T */
   for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) if (C[i * 4 + j] != j * 4 + i) return 11;
   RISCV_AME_RELEASE();
   CSRR(v, AMEOWN);    if (v != 0) return 12;
   return 100;
}
