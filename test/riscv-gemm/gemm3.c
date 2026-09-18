/* gemm3: one matrix product, three ways, self-verifying.
 *
 *   C = A x B^T,  A is M x K, B is N x K, C is M x N, int32
 *
 * computed by a scalar reference, by RVV 1.0 (a vector dot product per
 * element: vle32/vmul/vredsum), and by IME 0.9 (2x2 tiles: vmtl.v,
 * vmmacc.vv, vmts.v through include/riscv_insn.h).  The three results are
 * compared and the exit code says what agreed:
 *
 *   100  all agree           5  RVV differs from scalar
 *     6  IME differs         8  AME differs (-DUSE_AME); sums for combinations
 *
 * Freestanding (-nostdlib, entry _start).  Platform: PLATFORM_HTIF makes
 * exit write the sail-riscv model's tohost word instead of the Linux
 * exit_group ecall, and NO_IME leaves the matrix path out (the Sail model
 * has no IME), so the same source runs under QEMU linux-user and under
 * the Sail RVV 1.0 model.
 *
 * Geometry (VLEN=128, SEW=32): the RVV path sets e32,m2 for vl=K=8; the IME
 * path sets e32,m1, vl=4 and lambda=2, so a tile is 2x2 elements; the
 * leading dimension of every matrix is K elements. */

#include <stdint.h>
#if !defined(NO_IME) || defined(USE_AME)
#include "riscv_insn.h"
#endif

#ifndef M
#define M 8
#endif
#ifndef N
#define N 8
#endif
#ifndef K
#define K 8
#endif

static int32_t A[M * K] __attribute__((aligned(64)));
static int32_t B[N * K] __attribute__((aligned(64)));
static int32_t C_ref[M * N] __attribute__((aligned(64)));
static int32_t C_rvv[M * N] __attribute__((aligned(64)));
static int32_t C_ime[M * N] __attribute__((aligned(64)));
static int32_t C_ame[M * N] __attribute__((aligned(64)));

#ifdef PLATFORM_HTIF
volatile uint64_t tohost __attribute__((section(".tohost"), aligned(64)));
volatile uint64_t fromhost __attribute__((section(".tohost"), aligned(64)));
static void sys_exit(long code)
{
   /* HTIF exit: the model reports SUCCESS for 0, so 100 maps to 0 */
   tohost = ((uint64_t)(code == 100 ? 0 : code) << 1) | 1;
   for (;;)
      ;
}
/* Bare metal starts with the vector and FP units off in mstatus (VS, FS);
 * turn them on (Initial) before the first vector instruction. */
static void platform_init(void)
{
   __asm__ volatile("li t0, (1 << 9) | (1 << 13)\n\tcsrs mstatus, t0" : : : "t0");
}
#else
static void platform_init(void) {}
static void sys_exit(long code)
{
   register long a7 __asm__("a7") = 94;          /* exit_group */
   register long a0 __asm__("a0") = code;
   __asm__ volatile("ecall" : : "r"(a7), "r"(a0));
   for (;;)
      ;
}
#endif

static void fill(void)
{
   /* small deterministic values, signed, with products that stay in range */
   for (int i = 0; i < M * K; i++) A[i] = (int32_t)((i * 7) % 11) - 5;
   for (int i = 0; i < N * K; i++) B[i] = (int32_t)((i * 5) % 13) - 6;
}

static void gemm_scalar(void)
{
   for (int i = 0; i < M; i++)
      for (int j = 0; j < N; j++)
      {
         int32_t s = 0;
         for (int k = 0; k < K; k++)
            s += A[i * K + k] * B[j * K + k];
         C_ref[i * N + j] = s;
      }
}

/* RVV 1.0: each C element is a K-long dot product.  Two unit-stride loads,
 * an element-wise multiply, a reduction, and the scalar moved out. */
static void gemm_rvv(void)
{
   for (int i = 0; i < M; i++)
      for (int j = 0; j < N; j++)
      {
         int32_t s;
         const int32_t *a = &A[i * K], *b = &B[j * K];
         long n = K;
         __asm__ volatile(
            "vsetvli t0, zero, e32, m2, ta, ma\n\t"
            "vmv.v.i v6, 0\n\t"
            "1:\n\t"
            "vsetvli t0, %[n], e32, m2, tu, mu\n\t"       /* tail undisturbed: upper lanes keep their sums */
            "vle32.v v2, (%[a])\n\t"
            "vle32.v v4, (%[b])\n\t"
            "vmacc.vv v6, v2, v4\n\t"
            "slli t1, t0, 2\n\t"
            "add %[a], %[a], t1\n\t"
            "add %[b], %[b], t1\n\t"
            "sub %[n], %[n], t0\n\t"
            "bnez %[n], 1b\n\t"
            "vsetvli t0, zero, e32, m2, ta, ma\n\t"
            "vmv.s.x v8, zero\n\t"
            "vredsum.vs v8, v6, v8\n\t"
            "vmv.x.s %[s], v8"
            : [s] "=r"(s), [a] "+r"(a), [b] "+r"(b), [n] "+r"(n)
            : : "t0", "t1", "memory");
         C_rvv[i * N + j] = s;
      }
}

#ifndef NO_IME
/* IME 0.9: 2x2 tiles.  For each output tile, load it (zero), then for each
 * 2-wide K slice load the A and B tiles and accumulate C += A x B^T; store. */
static void gemm_ime(void)
{
   const long vl = 4, ld = K;
   long old;
   __asm__ volatile("vsetvli t0, %0, e32, m1, ta, ma" : : "r"(vl) : "t0", "memory");
   RISCV_VSETLAMBDA(old, 2L);
   (void)old;
   for (int ti = 0; ti < M; ti += 2)
      for (int tj = 0; tj < N; tj += 2)
      {
         RISCV_VMTL_V(16, &C_ime[ti * N + tj], (long)N, 2, 1);      /* C tile (zero) */
         for (int k = 0; k < K; k += 2)
         {
            RISCV_VMTL_V(0, &A[ti * K + k], ld, 2, 1);               /* A tile: rows ti,ti+1, cols k,k+1 */
            RISCV_VMTL_V(8, &B[tj * K + k], ld, 2, 1);               /* B tile: rows tj,tj+1, cols k,k+1 */
            RISCV_VMMACC_VV(16, 0, 8, 1);                            /* C += A x B^T */
         }
         RISCV_VMTS_V(16, &C_ime[ti * N + tj], (long)N, 2, 1);
      }
}
#endif

#ifdef USE_AME
/* AME (Ztt) draft, on the functional backend model in the QEMU frontend:
 * 4x4 Int32 squares.  A and B squares are loaded strided (rows K elements
 * apart), C = A x B^T accumulates with the transposed-B multiply, and each
 * finished square is moved to an M register and stored strided. */
static void gemm_ame(void)
{
   long status;
   RISCV_AME_ACQUIRE(status, 0L);                 /* TRY */
   const long dtype = 0x40000020L;                /* standard, signed, 32 bits */
   RISCV_MSETTYP(0, dtype); RISCV_MSETTYP(1, dtype); RISCV_MSETTYP(2, dtype);
   for (int ti = 0; ti < M; ti += 4)
      for (int tj = 0; tj < N; tj += 4)
      {
         RISCV_ASETTYP(0, dtype);                 /* zero the accumulator */
         for (int k = 0; k < K; k += 4)
         {
            RISCV_MLS_ST(0, &A[ti * K + k], (long)(K * 4));
            RISCV_MLS_ST(1, &B[tj * K + k], (long)(K * 4));
            RISCV_MMULBTACC_2D(0, 0, 1);          /* acc0 += A x B^T */
         }
         RISCV_MMOV_M_A(2, 0);
         RISCV_MSS_ST(2, &C_ame[ti * N + tj], (long)(N * 4));
      }
   RISCV_AME_RELEASE();
   (void)status;
}
#endif

static int same(const int32_t *x, const int32_t *y)
{
   for (int i = 0; i < M * N; i++)
      if (x[i] != y[i])
         return 0;
   return 1;
}

#ifdef PLATFORM_HTIF
/* Bare metal: nothing has set a stack when _start runs, and the compiler may
   spill registers in the first prologue, so the entry is assembly that points
   sp at a static stack before any C code. */
static char stack_area[8192] __attribute__((aligned(16), used));
__asm__(".section .text.start\n"
        ".globl _start\n"
        "_start:\n"
        "  lla sp, stack_area + 8192\n"
        "  j start_c\n");
void start_c(void)
#else
void __attribute__((section(".text.start"))) _start(void)
#endif
{
   platform_init();
   fill();
   gemm_scalar();
   gemm_rvv();
   int bad = same(C_ref, C_rvv) ? 0 : 1;
#ifndef NO_IME
   gemm_ime();
   bad |= same(C_ref, C_ime) ? 0 : 2;
#endif
#ifdef USE_AME
#ifndef AME_REPEAT
#define AME_REPEAT 1                              /* more repeats make the AME path dominate a timing run */
#endif
   for (int rep = 0; rep < AME_REPEAT; rep++)
   {
      gemm_ame();
      bad |= same(C_ref, C_ame) ? 0 : 4;
   }
#endif
   sys_exit(bad == 0 ? 100 : bad + 4);            /* 5 RVV, 6 IME, 8 AME, sums for combinations */
}
