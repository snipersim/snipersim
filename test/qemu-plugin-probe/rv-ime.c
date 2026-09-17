/* An IME 0.9 (Zvvm) guest for the QEMU frontend tests: one 2x2 integer
 * matrix multiply-accumulate on tiles loaded and stored with the
 * order-preserving tile instructions, then a check of the result.
 *
 * Freestanding, like rv-loop.c: built with -nostdlib and entered at _start.
 * The vector and matrix instructions are emitted as .word encodings so that
 * a compiler without the V or IME extensions can build it; the encodings are
 * the ones ime-0.9-pipeline's llvm-mc produces:
 *
 *   0d0572d7  vsetvli   t0, a0, e32, m1, ta, ma
 *   0205830b  vsetlambda t1, a1
 *   52b57007  vmtl.v    v0,  (a0), a1, L2
 *   52b57407  vmtl.v    v8,  (a0), a1, L2
 *   52b57807  vmtl.v    v16, (a0), a1, L2
 *   e2800857  vmmacc.vv v16, v0, v8
 *   52b57827  vmts.v    v16, (a0), a1, L2
 *
 * With VLEN=128 and SEW=32 a tile is 2x2 elements.  The leading dimension
 * is 16 elements (64 bytes), so the two rows of every tile lie in different
 * cache lines: each tile load or store touches two lines.
 *
 * Exits 100 when C = A x B^T comes out right, 5 otherwise. */

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

#define TILE_OP(word, base, ld)                                              \
   do {                                                                       \
      register long a0 __asm__("a0") = (long)(base);                          \
      register long a1 __asm__("a1") = (ld);                                  \
      __asm__ volatile(".word " #word : : "r"(a0), "r"(a1) : "memory");       \
   } while (0)

void _start(void)
{
   {
      register long a0 __asm__("a0") = 4;                 /* vl = 4 elements */
      __asm__ volatile(".word 0x0d0572d7" : : "r"(a0) : "t0", "memory");
   }
   {
      register long a1 __asm__("a1") = 2;                 /* lambda = 2 */
      __asm__ volatile(".word 0x0205830b" : : "r"(a1) : "t1", "memory");
   }
   TILE_OP(0x52b57007, A, LD);                            /* v0  <- A */
   TILE_OP(0x52b57407, B, LD);                            /* v8  <- B */
   TILE_OP(0x52b57807, C, LD);                            /* v16 <- C (zero) */
   __asm__ volatile(".word 0xe2800857" : : : "memory");   /* v16 += v0 x v8^T */
   TILE_OP(0x52b57827, OUT, LD);                          /* OUT <- v16 */

   volatile u32 *out = OUT;
   if (out[0] != EXPECTED[0] || out[1] != EXPECTED[1]
       || out[LD] != EXPECTED[2] || out[LD + 1] != EXPECTED[3])
      sys_exit(5);
   sys_exit(100);
}
