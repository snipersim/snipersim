/* An IME 0.9 (Zvvm) guest for the QEMU frontend tests: one 2x2 integer
 * matrix multiply-accumulate on tiles loaded and stored with the
 * order-preserving tile instructions, then a check of the result.
 *
 * Freestanding, like rv-loop.c: built with -nostdlib and entered at _start.
 * The matrix instructions come from include/riscv_insn.h, which
 * tools/riscv/gen_isa.py generates from the specification as typed .insn
 * directives, so an unmodified compiler emits them (-march=rv64gcv for the
 * standard vector configuration instruction).  The encodings are the ones
 * the ime-0.9-pipeline's llvm-mc produces:
 *
 *   vsetvli   t0, a0, e32, m1, ta, ma    (standard RVV)
 *   vsetlambda t1, a1                    0x0205830b-shaped, custom-0
 *   vmtl.v    v0/v8/v16, (a0), a1, L2    0x52b57007 / 0x52b57407 / 0x52b57807
 *   vmmacc.vv v16, v0, v8                0xe2800857
 *   vmts.v    v16, (a0), a1, L2          0x52b57827
 *
 * With VLEN=128 and SEW=32 a tile is 2x2 elements.  The leading dimension
 * is 16 elements (64 bytes), so the two rows of every tile lie in different
 * cache lines: each tile load or store touches two lines.
 *
 * Exits 100 when C = A x B^T comes out right, 5 otherwise. */

#include "riscv_insn.h"

typedef unsigned int u32;

#define LD 16

static u32 A[2 * LD] __attribute__((aligned(64))) = { [0] = 1, [1] = 2, [LD] = 3, [LD + 1] = 4 };
static u32 B[2 * LD] __attribute__((aligned(64))) = { [0] = 5, [1] = 6, [LD] = 7, [LD + 1] = 8 };
static u32 C[2 * LD] __attribute__((aligned(64)));
static u32 OUT[2 * LD] __attribute__((aligned(64)));
/* A x B^T: [1 2; 3 4] x [5 7; 6 8] */
static const u32 EXPECTED[4] = { 17, 23, 39, 53 };

static void sys_exit(long code)
{
   register long a7 __asm__("a7") = 93;
   register long a0 __asm__("a0") = code;
   __asm__ volatile("ecall" : : "r"(a7), "r"(a0));
   for (;;)
      ;
}

void _start(void)
{
   long vl = 4, old;
   __asm__ volatile("vsetvli t0, %0, e32, m1, ta, ma" : : "r"(vl) : "t0", "memory");
   RISCV_VSETLAMBDA(old, 2L);                          /* lambda = 2 */
   RISCV_VMTL_V(0, A, (long)LD, 2, 1);                 /* v0  <- A   (lambda L2, unmasked) */
   RISCV_VMTL_V(8, B, (long)LD, 2, 1);                 /* v8  <- B */
   RISCV_VMTL_V(16, C, (long)LD, 2, 1);                /* v16 <- C (zero) */
   RISCV_VMMACC_VV(16, 0, 8, 1);                       /* v16 += v0 x v8^T */
   RISCV_VMTS_V(16, OUT, (long)LD, 2, 1);              /* OUT <- v16 */

   volatile u32 *out = OUT;
   if (out[0] != EXPECTED[0] || out[1] != EXPECTED[1]
       || out[LD] != EXPECTED[2] || out[LD + 1] != EXPECTED[3])
      sys_exit(5);
   (void)old;
   sys_exit(100);
}
