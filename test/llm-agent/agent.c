/* agent: a tiny GPT that calls tools, running freestanding on RISC-V.
 *
 * The model (test/llm-agent/model_weights.h, trained on the host by
 * tools/llm/train_agent_gpt.py) is a nanoGPT-shaped character-level
 * transformer.  This runtime is the inference side only: a KV-cached
 * decoder, greedy sampling, and an agent loop --
 *
 *     Q: <question>\n T: <the model writes a tool call>\n
 *     R: <the runtime executes it and writes the result>\n
 *     A: <the model writes the answer>\n
 *
 * -- over the prompts in prompts.h.  Every generated line is checked against
 * the numpy model's own greedy output (the two implementations must agree
 * token for token) and the answer against the tool's true result.  Exit 100
 * when all prompts pass, 5 when a line differs from the numpy model, 6 when
 * the model's answer is wrong, 7 for both.
 *
 * Freestanding: no libc; write(2) for output, exit_group(2) to leave.  Math
 * (exp, tanh) is implemented here so the edge build needs no libm; sqrt is
 * the F-extension instruction.  The dot-product kernel is selectable:
 * scalar (default) or RVV 1.0 (-DUSE_RVV: vfmacc.vv over e32,m4 groups, a
 * vfredusum.vs at the end), the first optimization lever.
 * The weight matvecs can go to a matrix unit instead: -DUSE_IME (IME 0.9
 * fp32 2x2 tiles, vfmmacc.vv, the activation vector as the top row of the
 * B tile over a zero row) or -DUSE_AME (the AME draft, fp32 4x4 squares,
 * mmulbtacc.2d, the vector as the top row of a square); both keep the RVV
 * dot product for attention.  Decode is batch 1: the unit computes N rows
 * of its tile for one useful column.  Prefill runs the queued prompt text
 * as one batch (forward_batch), where each weight matvec becomes a GEMM
 * over the token block and the tiles fill: the class B shape. */

#include <stdint.h>
#include "sim_api.h"          /* the region of interest: the inference, not the model's set-up */
#include "model_weights.h"
#ifdef GPT_W_BLOB
/* The weights as a binary blob embedded at link time (a 100 MiB model is not
   C text): the trainer's --blob writes model_weights.bin next to the header,
   found through the include path.  A quoted #include searches the source
   file's own directory first, so a model in another directory is built from
   a copy of this file placed outside test/llm-agent. */
__asm__(".section .rodata\n.balign 64\n.globl GPT_W\nGPT_W:\n.incbin \"" GPT_W_BLOB "\"\n.previous");
extern const float GPT_W[];
#endif
#if defined(USE_W4)
#include "model_w4.h"         /* the matmul weights as int4 rows (tools/llm/quantize_w4.py) */
#include "prompts_w4.h"       /* the int4 numpy model's own outputs */
__asm__(".section .rodata\n.balign 64\n.globl W4_BLOB\nW4_BLOB:\n.incbin \"" W4_BLOB_FILE "\"\n.previous");
extern const unsigned char W4_BLOB[];
#else
#include "prompts.h"
#endif
#ifndef GEN_CALL
#define GEN_CALL 40           /* tokens per generate; a model's header pins both to the numpy value it was referenced with */
#define GEN_ANS 24
#endif

/* ---------------------------------------------------------- freestanding */
void *memcpy(void *d, const void *s, unsigned long n) { char *dd = d; const char *ss = s; while (n--) *dd++ = *ss++; return d; }
void *memset(void *d, int c, unsigned long n) { char *dd = d; while (n--) *dd++ = (char)c; return d; }
void *memmove(void *d, const void *s, unsigned long n) { char *dd = d; const char *ss = s; if (dd < ss) while (n--) *dd++ = *ss++; else { dd += n; ss += n; while (n--) *--dd = *--ss; } return d; }
static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void out(const char *s)
{
   register long a7 __asm__("a7") = 64, a0 __asm__("a0") = 1;
   register const char *a1 __asm__("a1") = s;
   register long a2 __asm__("a2") = (long)slen(s);
   __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
}
static void outn(long v)
{
   char b[24]; int i = 23; b[i] = 0;
   int neg = v < 0; if (neg) v = -v;
   do { b[--i] = (char)('0' + (v % 10)); v /= 10; } while (v);
   if (neg) b[--i] = '-';
   out(b + i);
}
static void sys_exit(long code)
{
   register long a7 __asm__("a7") = 94, a0 __asm__("a0") = code;
   __asm__ volatile("ecall" : : "r"(a7), "r"(a0));
   for (;;) ;
}

/* ---------------------------------------------------------- math */
static float expf_(float x)
{
   /* e^x = 2^k * e^r with |r| <= ln2/2, degree-6 polynomial; clamped */
   if (x > 88.0f) x = 88.0f;
   if (x < -87.0f) return 0.0f;
   const float ln2 = 0.6931471805599453f, inv = 1.4426950408889634f;
   int k = (int)(x * inv + (x >= 0 ? 0.5f : -0.5f));
   float r = x - (float)k * ln2;
   float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f + r * (0.041666668f + r * (0.008333334f + r * 0.0013888889f)))));
   union { uint32_t u; float f; } s; s.u = (uint32_t)(k + 127) << 23;
   return p * s.f;
}
static float tanhf_(float x)
{
   if (x > 9.0f) return 1.0f;
   if (x < -9.0f) return -1.0f;
   float e = expf_(2.0f * x);
   return (e - 1.0f) / (e + 1.0f);
}
static float sqrtf_(float x) { float r; __asm__("fsqrt.s %0, %1" : "=f"(r) : "f"(x)); return r; }

/* ---------------------------------------------------------- kernels */
#if defined(USE_IME) || defined(USE_AME) || defined(USE_DMA) || defined(USE_W4)
#include "riscv_insn.h"
#define USE_RVV
#endif
#ifdef USE_RVV
static float dot(const float *a, const float *b, long n)
{
   float s;
   __asm__ volatile(
      "vsetvli t0, zero, e32, m4, ta, ma\n\t"
      "vmv.v.i v8, 0\n\t"
      "1:\n\t"
      "vsetvli t0, %[n], e32, m4, tu, mu\n\t"      /* tail undisturbed: upper accumulator lanes keep their sums */
      "vle32.v v0, (%[a])\n\t"
      "vle32.v v4, (%[b])\n\t"
      "vfmacc.vv v8, v0, v4\n\t"
      "slli t1, t0, 2\n\t"
      "add %[a], %[a], t1\n\t"
      "add %[b], %[b], t1\n\t"
      "sub %[n], %[n], t0\n\t"
      "bnez %[n], 1b\n\t"
      "vsetvli t0, zero, e32, m4, ta, ma\n\t"
      "vmv.s.x v12, zero\n\t"
      "vfredusum.vs v12, v8, v12\n\t"
      "vfmv.f.s %[s], v12"
      : [s] "=f"(s), [a] "+r"(a), [b] "+r"(b), [n] "+r"(n) : : "t0", "t1", "memory");
   return s;
}
#define MATVEC_NAME "rvv"
#else
static float dot(const float *a, const float *b, long n)
{
   float s = 0.0f;
   for (long i = 0; i < n; i++) s += a[i] * b[i];
   return s;
}
#define MATVEC_NAME "scalar"
#endif

/* y[o] = bias[o] + dot(x, W[o]) for o < nout; W is [nout][nin] */
#if defined(USE_DMA)
#undef MATVEC_NAME
#define MATVEC_NAME "dma"
/* The DMA-attached accelerator (Xsnacc): every weight product is a task
 * described in memory; the core issues it and waits for the result. */
struct acc_desc { const float *x, *w, *bias; float *y; unsigned t, nin, nout, pad; };
static struct acc_desc acc_d __attribute__((aligned(64)));
static void acc_run(float *y, const float *x, const float *W, const float *bias, int T, int nin, int nout)
{
   acc_d.x = x; acc_d.w = W; acc_d.bias = bias; acc_d.y = y; acc_d.t = (unsigned)T; acc_d.nin = (unsigned)nin; acc_d.nout = (unsigned)nout;
   RISCV_ACC_TASK(&acc_d);
   RISCV_ACC_WAIT();
}
static void matvec(float *y, const float *x, const float *W, const float *bias, int nin, int nout)
{
   acc_run(y, x, W, bias, 1, nin, nout);
}
#elif !defined(USE_IME) && !defined(USE_AME)
static void matvec(float *y, const float *x, const float *W, const float *bias, int nin, int nout)
{
   for (int o = 0; o < nout; o++)
      y[o] = (bias ? bias[o] : 0.0f) + dot(x, W + (long)o * nin, nin);
}
#elif defined(USE_IME)
#undef MATVEC_NAME
#define MATVEC_NAME "ime"
/* IME tile geometry: rows = (VLEN/SEW)/lambda, cols = lambda*LMUL; square
 * tiles of side IME_S (2 at VLEN=128 e32 m1 lambda 2; 32 at VLEN=4096 e32 m8
 * lambda 4).  A tile is v0, B v8, the accumulators start at v16, one register
 * group of LMUL registers each: 16/LMUL of them. */
#ifndef IME_VLEN
#define IME_VLEN 128
#endif
#ifndef IME_LMUL
#define IME_LMUL 1
#endif
#ifndef IME_LAMBDA
#define IME_LAMBDA 2
#endif
#define IME_S (IME_LAMBDA * IME_LMUL)
#define IME_NACC (16 / IME_LMUL)
/* The lambda field of the tile instructions is log2(lambda)+1; it must reach
   the .insn macro as a literal, hence the #if chain and the two-level macros
   (the header stringifies its argument before expansion). */
#if IME_LAMBDA == 1
#define IME_LAMBDA_ENC 1
#elif IME_LAMBDA == 2
#define IME_LAMBDA_ENC 2
#elif IME_LAMBDA == 4
#define IME_LAMBDA_ENC 3
#elif IME_LAMBDA == 8
#define IME_LAMBDA_ENC 4
#elif IME_LAMBDA == 16
#define IME_LAMBDA_ENC 5
#else
#define IME_LAMBDA_ENC 6
#endif
#define IME_VMTL_(vd, p, ld, enc) RISCV_VMTL_V(vd, p, ld, enc, 1)
#define IME_VMTS_(vs, p, ld, enc) RISCV_VMTS_V(vs, p, ld, enc, 1)
#define IME_VMTL(vd, p, ld) IME_VMTL_(vd, p, ld, IME_LAMBDA_ENC)
#define IME_VMTS(vs, p, ld) IME_VMTS_(vs, p, ld, IME_LAMBDA_ENC)
#define IME_STR_(x) #x
#define IME_STR(x) IME_STR_(x)
_Static_assert((IME_VLEN / 32) / IME_LAMBDA == IME_S, "agent.c uses square IME tiles: (VLEN/32)/lambda must equal lambda*LMUL");
_Static_assert(GPT_T % IME_S == 0 && GPT_D % IME_S == 0, "the window and the width must be multiples of the IME tile");
#define IME_VSETVLI() __asm__ volatile("vsetvli t0, zero, e32, m" IME_STR(IME_LMUL) ", ta, ma" : : : "t0")
static float xpad[IME_S][4 * GPT_D];             /* row 0: the activation vector, the rest zeros */
/* Per IME_S output rows: C += A x B^T over IME_S-wide K slices, A = IME_S rows
 * of W, B = x over zero rows, so column 0 of C is y[o..o+IME_S-1]. */
static void matvec(float *y, const float *x, const float *W, const float *bias, int nin, int nout)
{
   long old;
   for (int i = 0; i < nin; i++) xpad[0][i] = x[i];
   IME_VSETVLI();
   RISCV_VSETLAMBDA(old, (long)IME_LAMBDA); (void)old;
   int o = 0;
   for (; o + IME_S <= nout; o += IME_S)
   {
      float t[IME_S * IME_S];
      __asm__ volatile("vmv.v.i v16, 0");                                  /* C tile */
      for (int k = 0; k < nin; k += IME_S)
      {
         IME_VMTL(0, W + (long)o * nin + k, (long)nin);      /* A: rows o.., cols k.. */
         IME_VMTL(8, &xpad[0][k], (long)(4 * GPT_D));        /* B: x[k..] over zeros */
         RISCV_VFMMACC_VV(16, 0, 8, 1);                                             /* C += A x B^T */
      }
      IME_VMTS(16, t, (long)IME_S);
      for (int i = 0; i < IME_S; i++) y[o + i] = (bias ? bias[o + i] : 0.0f) + t[i * IME_S];
   }
   for (; o < nout; o++) y[o] = (bias ? bias[o] : 0.0f) + dot(x, W + (long)o * nin, nin);
}
#else
#undef MATVEC_NAME
#define MATVEC_NAME "ame"
#define AME_FP32 0x20300120L                     /* IEEE class: exp 8, RNE, Inf, denormals, 32 bits */
/* The AME square side N = sqrt(AME_NELEM), an implementation parameter of the
 * draft (any power of 4 elements): 4 by default, the plugin model takes the
 * same value as ame_n=<N>. */
#ifndef AME_N
#define AME_N 4
#endif
_Static_assert(GPT_T % AME_N == 0 && GPT_D % AME_N == 0, "the window and the width must be multiples of the AME square");
static float xpad[AME_N][4 * GPT_D];             /* row 0: the activation vector, the rest zeros */
/* Per N output rows: acc(NxN) += A(NxN) x B(NxN)^T over N-wide K slices, A =
 * N rows of W (strided square), B = x over N-1 zero rows, so column 0 of the
 * accumulator is y[o..o+N-1]. */
static void matvec(float *y, const float *x, const float *W, const float *bias, int nin, int nout)
{
   for (int i = 0; i < nin; i++) xpad[0][i] = x[i];
   int o = 0;
   for (; o + AME_N <= nout; o += AME_N)
   {
      float t[AME_N][AME_N];
      RISCV_ASETTYP(0, AME_FP32);                                          /* zero the accumulator */
      for (int k = 0; k < nin; k += AME_N)
      {
         RISCV_MLS_ST(0, W + (long)o * nin + k, (long)nin * 4);            /* A: rows o.., cols k.. */
         RISCV_MLS_ST(1, &xpad[0][k], (long)(4 * GPT_D) * 4);              /* B: x[k..] over zeros */
         RISCV_MMULBTACC_2D(0, 0, 1);                                       /* acc0 += A x B^T */
      }
      RISCV_MMOV_M_A(2, 0);
      RISCV_MSS_ST(2, &t[0][0], (long)AME_N * 4);
      for (int i = 0; i < AME_N; i++) y[o + i] = (bias ? bias[o + i] : 0.0f) + t[i][0];
   }
   for (; o < nout; o++) y[o] = (bias ? bias[o] : 0.0f) + dot(x, W + (long)o * nin, nin);
}
#endif
#ifdef USE_RVV
/* Layer normalisation and GELU on the vector unit (e32, m4 strips of whatever
 * length vsetvli grants, so the same code runs at every VLEN).  Both keep the
 * scalar arithmetic: two reductions and a normalising pass; GELU's tanh
 * through the same clamped range reduction and degree-6 polynomial as expf_.
 * The harness derives every vector-class count of the RVV build from these
 * loops (testing/test_qemu.py, llm_vector_counts). */
static void layernorm(float *y, const float *x, const float *g, const float *b, int n)
{
   float mu, var, rstd;
   long nn = n; const float *p = x;
   __asm__ volatile(                                            /* mu = sum x */
      "vsetvli t0, zero, e32, m4, ta, ma\n\t"
      "vmv.v.i v8, 0\n\t"
      "1:\n\t"
      "vsetvli t0, %[n], e32, m4, tu, mu\n\t"
      "vle32.v v0, (%[p])\n\t"
      "vfadd.vv v8, v8, v0\n\t"
      "slli t1, t0, 2\n\t"
      "add %[p], %[p], t1\n\t"
      "sub %[n], %[n], t0\n\t"
      "bnez %[n], 1b\n\t"
      "vsetvli t0, zero, e32, m4, ta, ma\n\t"
      "vmv.s.x v12, zero\n\t"
      "vfredusum.vs v12, v8, v12\n\t"
      "vfmv.f.s %[s], v12"
      : [s] "=f"(mu), [p] "+r"(p), [n] "+r"(nn) : : "t0", "t1", "memory");
   mu /= (float)n;
   nn = n; p = x;
   __asm__ volatile(                                            /* var = sum (x - mu)^2 */
      "vsetvli t0, zero, e32, m4, ta, ma\n\t"
      "vmv.v.i v8, 0\n\t"
      "1:\n\t"
      "vsetvli t0, %[n], e32, m4, tu, mu\n\t"
      "vle32.v v0, (%[p])\n\t"
      "vfsub.vf v0, v0, %[mu]\n\t"
      "vfmacc.vv v8, v0, v0\n\t"
      "slli t1, t0, 2\n\t"
      "add %[p], %[p], t1\n\t"
      "sub %[n], %[n], t0\n\t"
      "bnez %[n], 1b\n\t"
      "vsetvli t0, zero, e32, m4, ta, ma\n\t"
      "vmv.s.x v12, zero\n\t"
      "vfredusum.vs v12, v8, v12\n\t"
      "vfmv.f.s %[s], v12"
      : [s] "=f"(var), [p] "+r"(p), [n] "+r"(nn) : [mu] "f"(mu) : "t0", "t1", "memory");
   var /= (float)n;
   rstd = 1.0f / sqrtf_(var + 1e-5f);
   nn = n; p = x;
   __asm__ volatile(                                            /* y = (x - mu) * rstd * g + b */
      "1:\n\t"
      "vsetvli t0, %[n], e32, m4, ta, ma\n\t"
      "vle32.v v0, (%[p])\n\t"
      "vfsub.vf v0, v0, %[mu]\n\t"
      "vfmul.vf v0, v0, %[rstd]\n\t"
      "vle32.v v4, (%[g])\n\t"
      "vfmul.vv v0, v0, v4\n\t"
      "vle32.v v4, (%[b])\n\t"
      "vfadd.vv v0, v0, v4\n\t"
      "vse32.v v0, (%[y])\n\t"
      "slli t1, t0, 2\n\t"
      "add %[p], %[p], t1\n\t"
      "add %[g], %[g], t1\n\t"
      "add %[b], %[b], t1\n\t"
      "add %[y], %[y], t1\n\t"
      "sub %[n], %[n], t0\n\t"
      "bnez %[n], 1b"
      : [p] "+r"(p), [g] "+r"(g), [b] "+r"(b), [y] "+r"(y), [n] "+r"(nn) : [mu] "f"(mu), [rstd] "f"(rstd) : "t0", "t1", "memory");
}
/* GELU over n elements in place: u = c1 (x + c2 x^3), y = x (1 + tanh u) / 2,
 * tanh u = (e - 1) / (e + 1) with e = exp(2u), 2u clamped to [-20, 20] (the
 * scalar code returns +-1 beyond |u| = 9; e^20 rounds to the same). */
static void gelu_v(float *v, int n)
{
   long nn = n;
   const float c1x2 = 2.0f * 0.7978845608028654f, c2 = 0.044715f, hi = 20.0f, lo = -20.0f;
   const float inv = 1.4426950408889634f, ln2 = 0.6931471805599453f, half = 0.5f, one = 1.0f;
   const float c6 = 0.0013888889f, c5 = 0.008333334f, c4 = 0.041666668f, c3 = 0.16666667f;
   const long b127 = 127;
   __asm__ volatile(
      "1:\n\t"
      "vsetvli t0, %[n], e32, m4, ta, ma\n\t"
      "vle32.v v0, (%[p])\n\t"                 /* x */
      "vfmul.vv v4, v0, v0\n\t"                /* x^2 */
      "vfmul.vv v4, v4, v0\n\t"                /* x^3 */
      "vmv.v.v v8, v0\n\t"
      "vfmacc.vf v8, %[c2], v4\n\t"            /* x + c2 x^3 */
      "vfmul.vf v8, v8, %[c1x2]\n\t"           /* z = 2u */
      "vfmin.vf v8, v8, %[hi]\n\t"
      "vfmax.vf v8, v8, %[lo]\n\t"
      "vfmul.vf v4, v8, %[inv]\n\t"            /* z / ln2 */
      "vfcvt.x.f.v v12, v4\n\t"                /* k */
      "vfcvt.f.x.v v4, v12\n\t"
      "vfnmsac.vf v8, %[ln2], v4\n\t"          /* r = z - k ln2 */
      "vfmul.vf v4, v8, %[c6]\n\t"             /* p = c6 r + c5, Horner down to 1 */
      "vfadd.vf v4, v4, %[c5]\n\t"
      "vfmul.vv v4, v4, v8\n\t"
      "vfadd.vf v4, v4, %[c4]\n\t"
      "vfmul.vv v4, v4, v8\n\t"
      "vfadd.vf v4, v4, %[c3]\n\t"
      "vfmul.vv v4, v4, v8\n\t"
      "vfadd.vf v4, v4, %[half]\n\t"
      "vfmul.vv v4, v4, v8\n\t"
      "vfadd.vf v4, v4, %[one]\n\t"
      "vfmul.vv v4, v4, v8\n\t"
      "vfadd.vf v4, v4, %[one]\n\t"
      "vadd.vx v12, v12, %[b127]\n\t"          /* 2^k as float bits */
      "vsll.vi v12, v12, 23\n\t"
      "vfmul.vv v4, v4, v12\n\t"               /* e = p 2^k */
      "vfsub.vf v8, v4, %[one]\n\t"
      "vfadd.vf v4, v4, %[one]\n\t"
      "vfdiv.vv v8, v8, v4\n\t"                /* tanh */
      "vfadd.vf v8, v8, %[one]\n\t"
      "vfmul.vv v8, v8, v0\n\t"
      "vfmul.vf v8, v8, %[half]\n\t"
      "vse32.v v8, (%[p])\n\t"
      "slli t1, t0, 2\n\t"
      "add %[p], %[p], t1\n\t"
      "sub %[n], %[n], t0\n\t"
      "bnez %[n], 1b"
      : [p] "+r"(v), [n] "+r"(nn)
      : [c1x2] "f"(c1x2), [c2] "f"(c2), [hi] "f"(hi), [lo] "f"(lo), [inv] "f"(inv), [ln2] "f"(ln2),
        [c6] "f"(c6), [c5] "f"(c5), [c4] "f"(c4), [c3] "f"(c3), [half] "f"(half), [one] "f"(one), [b127] "r"(b127)
      : "t0", "t1", "memory");
}
#define GELU_INPLACE(v, n) gelu_v(v, n)
#else
static void layernorm(float *y, const float *x, const float *g, const float *b, int n)
{
   float mu = 0, var = 0;
   for (int i = 0; i < n; i++) mu += x[i];
   mu /= (float)n;
   for (int i = 0; i < n; i++) { float c = x[i] - mu; var += c * c; }
   var /= (float)n;
   float rstd = 1.0f / sqrtf_(var + 1e-5f);
   for (int i = 0; i < n; i++) y[i] = (x[i] - mu) * rstd * g[i] + b[i];
}
static float gelu(float x)
{
   float u = 0.7978845608028654f * (x + 0.044715f * x * x * x);
   return 0.5f * x * (1.0f + tanhf_(u));
}
#define GELU_INPLACE(v, n) do { for (int i_ = 0; i_ < (n); i_++) (v)[i_] = gelu((v)[i_]); } while (0)
#endif


/* ---------------------------------------------------------- prefill: the GEMM
 * Y[t][o] = bias[o] + sum_k Xm[t][k] * W[o][k] for T tokens at once (Y and
 * Xm with their own row strides).  The class B shape: the T x nin activation
 * block against the nout x nin weights, so a matrix unit fills every column
 * of its tile and a weight tile is loaded once per group of token tiles
 * instead of once per token. */
/* The accumulator a block uses is a register number, which must reach the
 * instruction as a literal.  A step over the nb blocks of a group is one
 * switch on nb that falls through: block nb-1 first, down to block 0, each
 * with its own literal (one indirect jump per step, not one per block). */

#if defined(USE_IME)
/* IME_S x IME_S tiles: a token block x an output block; the IME_NACC
 * accumulator groups (v16.. in steps of LMUL) hold that many token tiles, so a
 * weight tile is loaded once per IME_NACC*IME_S tokens.  A batch shorter than
 * a tile is padded: the rows beyond T come from the buffer (GPT_T rows) and
 * are never read back. */
/* per block i on accumulator group a: zero it, load the X tile and multiply-accumulate, store it */
#define IME_ZERO_B(a, i)  __asm__ volatile("vmv.v.i v" #a ", 0")
#define IME_MMA_B(a, i)   do { IME_VMTL(0, Xm + (long)(g + (i)) * IME_S * ldx + k, (long)ldx); RISCV_VFMMACC_VV(a, 0, 8, 1); } while (0)
#define IME_STORE_B(a, i) do { float *yp = Y + (long)(g + (i)) * IME_S * ldy + o; IME_VMTS(a, yp, (long)ldy); \
   if (bias) for (int r = 0; r < IME_S; r++) for (int c = 0; c < IME_S; c++) yp[r * ldy + c] += bias[o + c]; } while (0)
#if IME_LMUL == 1
#define IME_BLOCKS(nb, OP) do { switch (nb) { case 16: OP(31, 15); case 15: OP(30, 14); case 14: OP(29, 13); case 13: OP(28, 12); case 12: OP(27, 11); case 11: OP(26, 10); case 10: OP(25, 9); case 9: OP(24, 8); case 8: OP(23, 7); case 7: OP(22, 6); case 6: OP(21, 5); case 5: OP(20, 4); case 4: OP(19, 3); case 3: OP(18, 2); case 2: OP(17, 1); case 1: OP(16, 0); default: break; } } while (0)
#elif IME_LMUL == 2
#define IME_BLOCKS(nb, OP) do { switch (nb) { case 8: OP(30, 7); case 7: OP(28, 6); case 6: OP(26, 5); case 5: OP(24, 4); case 4: OP(22, 3); case 3: OP(20, 2); case 2: OP(18, 1); case 1: OP(16, 0); default: break; } } while (0)
#elif IME_LMUL == 4
#define IME_BLOCKS(nb, OP) do { switch (nb) { case 4: OP(28, 3); case 3: OP(24, 2); case 2: OP(20, 1); case 1: OP(16, 0); default: break; } } while (0)
#else
#define IME_BLOCKS(nb, OP) do { switch (nb) { case 2: OP(24, 1); case 1: OP(16, 0); default: break; } } while (0)
#endif
static void gemm(float *Y, int ldy, const float *Xm, int ldx, const float *W, const float *bias, int T, int nin, int nout)
{
   long old;
   const int TB = (T + IME_S - 1) / IME_S;
   IME_VSETVLI();
   RISCV_VSETLAMBDA(old, (long)IME_LAMBDA); (void)old;
   int o = 0;
   for (; o + IME_S <= nout; o += IME_S)
      for (int g = 0; g < TB; g += IME_NACC)
      {
         const int nb = TB - g < IME_NACC ? TB - g : IME_NACC;
         IME_BLOCKS(nb, IME_ZERO_B);
         for (int k = 0; k < nin; k += IME_S)
         {
            IME_VMTL(8, W + (long)o * nin + k, (long)nin);           /* W tile: rows o.. */
            IME_BLOCKS(nb, IME_MMA_B);                                /* per block: X tile of IME_S tokens, C += X x W^T */
         }
         IME_BLOCKS(nb, IME_STORE_B);
      }
   for (; o < nout; o++)
      for (int t = 0; t < T; t++) Y[(long)t * ldy + o] = (bias ? bias[o] : 0.0f) + dot(Xm + (long)t * ldx, W + (long)o * nin, nin);
}
#elif defined(USE_AME)
/* N x N squares: a token block x an output block; the 16 accumulators hold 16
 * token squares (16*N tokens), so a weight square is loaded once per prefill.
 * A batch shorter than a square is padded: the rows beyond T come from the
 * buffer (GPT_T rows) and are never read back. */
/* per block i on accumulator a: zero it, load the X square and multiply-accumulate, move it out and store */
#define AME_SET_B(a, i) RISCV_ASETTYP(a, AME_FP32)
#define AME_MMA_B(a, i) do { RISCV_MLS_ST(0, Xm + (long)(g + (i)) * AME_N * ldx + k, (long)ldx * 4); RISCV_MMULBTACC_2D(a, 0, 1); } while (0)
#define AME_MOV_B(a, i) do { float *yp = Y + (long)(g + (i)) * AME_N * ldy + o; RISCV_MMOV_M_A(2, a); RISCV_MSS_ST(2, yp, (long)ldy * 4); \
   if (bias) for (int r = 0; r < AME_N; r++) for (int c = 0; c < AME_N; c++) yp[r * ldy + c] += bias[o + c]; } while (0)
#define AME_BLOCKS(nb, OP) do { switch (nb) { case 16: OP(15, 15); case 15: OP(14, 14); case 14: OP(13, 13); case 13: OP(12, 12); case 12: OP(11, 11); case 11: OP(10, 10); case 10: OP(9, 9); case 9: OP(8, 8); case 8: OP(7, 7); case 7: OP(6, 6); case 6: OP(5, 5); case 5: OP(4, 4); case 4: OP(3, 3); case 3: OP(2, 2); case 2: OP(1, 1); case 1: OP(0, 0); default: break; } } while (0)
static void gemm(float *Y, int ldy, const float *Xm, int ldx, const float *W, const float *bias, int T, int nin, int nout)
{
   const int TB = (T + AME_N - 1) / AME_N;
   int o = 0;
   for (; o + AME_N <= nout; o += AME_N)
      for (int g = 0; g < TB; g += 16)
      {
         const int nb = TB - g < 16 ? TB - g : 16;
         AME_BLOCKS(nb, AME_SET_B);                                                /* zero the accumulators */
         for (int k = 0; k < nin; k += AME_N)
         {
            RISCV_MLS_ST(1, W + (long)o * nin + k, (long)nin * 4);                 /* W square: rows o.. */
            AME_BLOCKS(nb, AME_MMA_B);                                             /* per block: X square of N tokens, acc += X x W^T */
         }
         AME_BLOCKS(nb, AME_MOV_B);
      }
   for (; o < nout; o++)
      for (int t = 0; t < T; t++) Y[(long)t * ldy + o] = (bias ? bias[o] : 0.0f) + dot(Xm + (long)t * ldx, W + (long)o * nin, nin);
}
#elif defined(USE_DMA)
static void gemm(float *Y, int ldy, const float *Xm, int ldx, const float *W, const float *bias, int T, int nin, int nout)
{
   /* the task takes contiguous blocks: the agent's buffers have ldx == nin and ldy == nout */
   if (ldx != nin || ldy != nout) { for (int t = 0; t < T; t++) matvec(Y + (long)t * ldy, Xm + (long)t * ldx, W, bias, nin, nout); return; }
   acc_run(Y, Xm, W, bias, T, nin, nout);
}
#else
static void gemm(float *Y, int ldy, const float *Xm, int ldx, const float *W, const float *bias, int T, int nin, int nout)
{
   for (int t = 0; t < T; t++) matvec(Y + (long)t * ldy, Xm + (long)t * ldx, W, bias, nin, nout);
}
#endif


#ifdef USE_W4
/* ---------------------------------------------------------- W4A16: int4 weights
 * Per output row: a float32 scale and nin/2 bytes of planar nibbles (q+8; the
 * low nibbles are the first half of the row, the high nibbles the second).
 * The dot product dequantizes on the vector unit: unpack, widen, subtract 8,
 * convert, multiply-accumulate with x; one scale per row at the end.  8x
 * fewer weight bytes than fp32, the same number of vector operations. */
struct w4_mat { const unsigned char *q; const float *scale; int nout, nin; };
#define W4_ENTRY(K, NOUT, NIN) { W4_BLOB + W4_OFF_Q_##K, (const float *)(W4_BLOB + W4_OFF_S_##K), NOUT, NIN }
#define W4_LAYER(l) { W4_ENTRY(WQKV##l, 3 * GPT_D, GPT_D), W4_ENTRY(WO##l, GPT_D, GPT_D), W4_ENTRY(W1##l, 4 * GPT_D, GPT_D), W4_ENTRY(W2##l, GPT_D, 4 * GPT_D) }
static const struct w4_mat W4_MATS[GPT_L][4] = {
   W4_LAYER(0),
#if GPT_L > 1
   W4_LAYER(1),
#endif
#if GPT_L > 2
   W4_LAYER(2),
#endif
#if GPT_L > 3
   W4_LAYER(3),
#endif
#if GPT_L > 4
   W4_LAYER(4),
#endif
#if GPT_L > 5
   W4_LAYER(5),
#endif
#if GPT_L > 6
   W4_LAYER(6),
#endif
#if GPT_L > 7
   W4_LAYER(7),
#endif
#if GPT_L > 8
   W4_LAYER(8),
#endif
#if GPT_L > 9
   W4_LAYER(9),
#endif
#if GPT_L > 10
   W4_LAYER(10),
#endif
#if GPT_L > 11
   W4_LAYER(11),
#endif
#if GPT_L > 12
   W4_LAYER(12),
#endif
#if GPT_L > 13
   W4_LAYER(13),
#endif
#if GPT_L > 14
   W4_LAYER(14),
#endif
#if GPT_L > 15
   W4_LAYER(15),
#endif
};
static long n_w4_matvec;
static void w4_matvec(float *y, const float *x, const struct w4_mat *m, const float *bias)
{
   const int nin = m->nin, half = nin / 2;
   for (int o = 0; o < m->nout; o++)
   {
      const unsigned char *row = m->q + (long)o * half;
      float s;
      __asm__ volatile("vsetvli t0, zero, e32, m8, ta, ma\n\tvmv.v.i v24, 0" : : : "t0");
      for (int j = 0; j < half; )
      {
         long vl;
         __asm__ volatile(
            "vsetvli %[vl], %[n], e8, m2, ta, ma\n\t"
            "vle8.v v0, (%[row])\n\t"                          /* vl bytes: two weights each */
            "vand.vi v2, v0, 15\n\t"                           /* low nibbles: weights j.. */
            "vsrl.vi v4, v0, 4\n\t"                            /* high nibbles: weights j + half.. */
            "vsetvli %[vl], %[n], e32, m8, tu, ma\n\t"         /* tail undisturbed: the accumulator's upper lanes keep their sums */
            "vzext.vf4 v8, v2\n\t"
            "vadd.vi v8, v8, -8\n\t"
            "vfcvt.f.x.v v8, v8\n\t"
            "vle32.v v16, (%[xl])\n\t"
            "vfmacc.vv v24, v8, v16\n\t"
            "vzext.vf4 v8, v4\n\t"
            "vadd.vi v8, v8, -8\n\t"
            "vfcvt.f.x.v v8, v8\n\t"
            "vle32.v v16, (%[xh])\n\t"
            "vfmacc.vv v24, v8, v16"
            : [vl] "=&r"(vl) : [n] "r"((long)(half - j)), [row] "r"(row + j), [xl] "r"(x + j), [xh] "r"(x + half + j) : "t0", "memory");
         j += (int)vl;
      }
      __asm__ volatile(
         "vsetvli t0, zero, e32, m8, ta, ma\n\t"
         "vmv.s.x v0, zero\n\t"
         "vfredusum.vs v0, v24, v0\n\t"
         "vfmv.f.s %[s], v0"
         : [s] "=f"(s) : : "t0");
      y[o] = (bias ? bias[o] : 0.0f) + s * m->scale[o];
   }
   n_w4_matvec++;
}
#endif

/* ---------------------------------------------------------- the model */
#define HD (GPT_D / GPT_H)
static float Kc[GPT_L][GPT_T][GPT_D], Vc[GPT_L][GPT_T][GPT_D];   /* KV cache */
static float x[GPT_D], h[GPT_D], qkv[3 * GPT_D], att[GPT_D], o[GPT_D], m1[4 * GPT_D], m2[GPT_D], logits[GPT_V];
static float scores[GPT_T];

/* ---------------------------------------------------------- attention over the KV cache
 * One head of one query against n_t cached keys and values: scores by the
 * dot product, softmax, the weighted sum of the value rows.  The vector
 * build does the softmax as strip loops over the n_t scores (a max
 * reduction, the exp of the shifted scores with a running sum, the same
 * range reduction and polynomial as GELU) and the weighted sum as one
 * multiply-accumulate of each value row into an HD-wide accumulator; the
 * scalar build is the original loop.  att_strips counts the softmax strips
 * for the harness's derived counts (they depend on the positions). */
static long n_att_dots, n_att_strips;
#ifdef USE_RVV
static float vmax_(const float *v, long n)
{
   float m; const float neg = -1e30f;
   __asm__ volatile(
      "vsetvli t0, zero, e32, m4, ta, ma\n\t"
      "vfmv.v.f v8, %[neg]\n\t"
      "1:\n\t"
      "vsetvli t0, %[n], e32, m4, tu, mu\n\t"
      "vle32.v v0, (%[p])\n\t"
      "vfmax.vv v8, v8, v0\n\t"
      "slli t1, t0, 2\n\t"
      "add %[p], %[p], t1\n\t"
      "sub %[n], %[n], t0\n\t"
      "bnez %[n], 1b\n\t"
      "vsetvli t0, zero, e32, m4, ta, ma\n\t"
      "vfredmax.vs v12, v8, v8\n\t"
      "vfmv.f.s %[m], v12"
      : [m] "=f"(m), [p] "+r"(v), [n] "+r"(n) : [neg] "f"(neg) : "t0", "t1", "memory");
   return m;
}
/* v[i] = exp(v[i] - mx) in place; returns the sum, counts the strips */
static float vexp_(float *v, long n, float mx, long *strips)
{
   float sum; long c = 0;
   const float hi = 88.0f, lo = -87.0f, inv = 1.4426950408889634f, ln2 = 0.6931471805599453f, half = 0.5f, one = 1.0f;
   const float c6 = 0.0013888889f, c5 = 0.008333334f, c4 = 0.041666668f, c3 = 0.16666667f;
   const long b127 = 127;
   __asm__ volatile(
      "vsetvli t0, zero, e32, m4, ta, ma\n\t"
      "vmv.v.i v16, 0\n\t"
      "1:\n\t"
      "vsetvli t0, %[n], e32, m4, tu, mu\n\t"
      "vle32.v v0, (%[p])\n\t"
      "vfsub.vf v8, v0, %[mx]\n\t"
      "vfmin.vf v8, v8, %[hi]\n\t"
      "vfmax.vf v8, v8, %[lo]\n\t"
      "vfmul.vf v4, v8, %[inv]\n\t"
      "vfcvt.x.f.v v12, v4\n\t"
      "vfcvt.f.x.v v4, v12\n\t"
      "vfnmsac.vf v8, %[ln2], v4\n\t"
      "vfmul.vf v4, v8, %[c6]\n\t"
      "vfadd.vf v4, v4, %[c5]\n\t"
      "vfmul.vv v4, v4, v8\n\t"
      "vfadd.vf v4, v4, %[c4]\n\t"
      "vfmul.vv v4, v4, v8\n\t"
      "vfadd.vf v4, v4, %[c3]\n\t"
      "vfmul.vv v4, v4, v8\n\t"
      "vfadd.vf v4, v4, %[half]\n\t"
      "vfmul.vv v4, v4, v8\n\t"
      "vfadd.vf v4, v4, %[one]\n\t"
      "vfmul.vv v4, v4, v8\n\t"
      "vfadd.vf v4, v4, %[one]\n\t"
      "vadd.vx v12, v12, %[b127]\n\t"
      "vsll.vi v12, v12, 23\n\t"
      "vfmul.vv v4, v4, v12\n\t"
      "vse32.v v4, (%[p])\n\t"
      "vfadd.vv v16, v16, v4\n\t"
      "addi %[c], %[c], 1\n\t"
      "slli t1, t0, 2\n\t"
      "add %[p], %[p], t1\n\t"
      "sub %[n], %[n], t0\n\t"
      "bnez %[n], 1b\n\t"
      "vsetvli t0, zero, e32, m4, ta, ma\n\t"
      "vmv.s.x v12, zero\n\t"
      "vfredusum.vs v12, v16, v12\n\t"
      "vfmv.f.s %[s], v12"
      : [s] "=f"(sum), [p] "+r"(v), [n] "+r"(n), [c] "+r"(c)
      : [mx] "f"(mx), [hi] "f"(hi), [lo] "f"(lo), [inv] "f"(inv), [ln2] "f"(ln2),
        [c6] "f"(c6), [c5] "f"(c5), [c4] "f"(c4), [c3] "f"(c3), [half] "f"(half), [one] "f"(one), [b127] "r"(b127)
      : "t0", "t1", "memory");
   *strips += c;
   return sum;
}
/* out[0..hd) = (sum_t p[t] * V[t][0..hd)) * rs, V rows stride floats apart */
static void vaxpy_(float *out, const float *p, const float *V, long stride, long n_t, long hd, float rs)
{
   for (long i = 0; i < hd; )
   {
      long vl, t = n_t; const float *vp = V + i, *pp = p;
      __asm__ volatile(
         "vsetvli %[vl], %[n], e32, m4, ta, ma\n\t"
         "vmv.v.i v8, 0\n\t"
         "1:\n\t"
         "flw ft0, (%[pp])\n\t"
         "vle32.v v0, (%[vp])\n\t"
         "vfmacc.vf v8, ft0, v0\n\t"
         "add %[vp], %[vp], %[sb]\n\t"
         "addi %[pp], %[pp], 4\n\t"
         "addi %[t], %[t], -1\n\t"
         "bnez %[t], 1b\n\t"
         "vfmul.vf v8, v8, %[rs]\n\t"
         "vse32.v v8, (%[out])"
         : [vl] "=&r"(vl), [vp] "+r"(vp), [pp] "+r"(pp), [t] "+r"(t)
         : [n] "r"(hd - i), [sb] "r"(stride * 4), [rs] "f"(rs), [out] "r"(out + i) : "ft0", "memory");
      i += vl;
   }
}
static void attend(float *out, const float *q, const float *K, const float *V, long n_t, float scale)
{
   for (long t = 0; t < n_t; t++) scores[t] = dot(q, K + t * GPT_D, HD) * scale;
   n_att_dots += n_t;
   const float mx = vmax_(scores, n_t);
   const float sum = vexp_(scores, n_t, mx, &n_att_strips);
   vaxpy_(out, scores, V, GPT_D, n_t, HD, 1.0f / sum);
}
#else
static void attend(float *out, const float *q, const float *K, const float *V, long n_t, float scale)
{
   float mx = -1e30f;
   for (long t = 0; t < n_t; t++) { scores[t] = dot(q, K + t * GPT_D, HD) * scale; if (scores[t] > mx) mx = scores[t]; }
   n_att_dots += n_t;
   float sum = 0;
   for (long t = 0; t < n_t; t++) { scores[t] = expf_(scores[t] - mx); sum += scores[t]; }
   for (int i = 0; i < HD; i++) out[i] = 0;
   for (long t = 0; t < n_t; t++) { float p = scores[t] / sum; for (int i = 0; i < HD; i++) out[i] += p * V[t * GPT_D + i]; }
}
#endif
static int pos;

#define W(off) (GPT_W + (off))
/* the per-layer weight offsets, one row per layer up to 16 (a deeper model adds rows here) */
_Static_assert(GPT_L <= 16, "agent.c carries offset rows for up to 16 layers");
#define LAYER(l) { OFF_LN1_G##l, OFF_LN1_B##l, OFF_WQKV##l, OFF_BQKV##l, OFF_WO##l, OFF_BO##l, OFF_LN2_G##l, OFF_LN2_B##l, OFF_W1##l, OFF_B1##l, OFF_W2##l, OFF_B2##l }
static const int LOFF[GPT_L][12] = {
   LAYER(0),
#if GPT_L > 1
   LAYER(1),
#endif
#if GPT_L > 2
   LAYER(2),
#endif
#if GPT_L > 3
   LAYER(3),
#endif
#if GPT_L > 4
   LAYER(4),
#endif
#if GPT_L > 5
   LAYER(5),
#endif
#if GPT_L > 6
   LAYER(6),
#endif
#if GPT_L > 7
   LAYER(7),
#endif
#if GPT_L > 8
   LAYER(8),
#endif
#if GPT_L > 9
   LAYER(9),
#endif
#if GPT_L > 10
   LAYER(10),
#endif
#if GPT_L > 11
   LAYER(11),
#endif
#if GPT_L > 12
   LAYER(12),
#endif
#if GPT_L > 13
   LAYER(13),
#endif
#if GPT_L > 14
   LAYER(14),
#endif
#if GPT_L > 15
   LAYER(15),
#endif
};

/* A layer's weight product: the int4 rows under -DUSE_W4, the fp32 matrix otherwise. */
#ifdef USE_W4
#define LAYER_MATVEC(y, xin, which, Wp, bp, nin, nout) w4_matvec(y, xin, &W4_MATS[l][which], bp)
#define LAYER_GEMM(Y, ldy, Xm, ldx, which, Wp, bp, T, nin, nout) do { for (int t_ = 0; t_ < (T); t_++) w4_matvec((Y) + (long)t_ * (ldy), (Xm) + (long)t_ * (ldx), &W4_MATS[l][which], bp); } while (0)
#else
#define LAYER_MATVEC(y, xin, which, Wp, bp, nin, nout) matvec(y, xin, Wp, bp, nin, nout)
#define LAYER_GEMM(Y, ldy, Xm, ldx, which, Wp, bp, T, nin, nout) gemm(Y, ldy, Xm, ldx, Wp, bp, T, nin, nout)
#endif

/* one token in at position pos, logits out */
static void forward(int tok)
{
   for (int i = 0; i < GPT_D; i++) x[i] = W(OFF_WTE)[tok * GPT_D + i] + W(OFF_WPE)[pos * GPT_D + i];
   const float scale = 1.0f / sqrtf_((float)HD);
   for (int l = 0; l < GPT_L; l++)
   {
      const int *L = LOFF[l];
      layernorm(h, x, W(L[0]), W(L[1]), GPT_D);
      LAYER_MATVEC(qkv, h, 0, W(L[2]), W(L[3]), GPT_D, 3 * GPT_D);
      for (int i = 0; i < GPT_D; i++) { Kc[l][pos][i] = qkv[GPT_D + i]; Vc[l][pos][i] = qkv[2 * GPT_D + i]; }
      for (int hh = 0; hh < GPT_H; hh++)
         attend(att + hh * HD, qkv + hh * HD, &Kc[l][0][hh * HD], &Vc[l][0][hh * HD], pos + 1, scale);
      LAYER_MATVEC(o, att, 1, W(L[4]), W(L[5]), GPT_D, GPT_D);
      for (int i = 0; i < GPT_D; i++) x[i] += o[i];
      layernorm(h, x, W(L[6]), W(L[7]), GPT_D);
      LAYER_MATVEC(m1, h, 2, W(L[8]), W(L[9]), GPT_D, 4 * GPT_D);
      GELU_INPLACE(m1, 4 * GPT_D);
      LAYER_MATVEC(m2, m1, 3, W(L[10]), W(L[11]), 4 * GPT_D, GPT_D);
      for (int i = 0; i < GPT_D; i++) x[i] += m2[i];
   }
   layernorm(h, x, W(OFF_LNF_G), W(OFF_LNF_B), GPT_D);
   matvec(logits, h, W(OFF_HEAD), 0, GPT_D, GPT_V);
   pos++;
}

/* T tokens in at positions pos..pos+T-1 as one batch, logits of the last
 * one out.  The same arithmetic as T calls of forward() (the numpy check
 * still holds), but every weight matvec is one GEMM over the token block. */
static float Xb[GPT_T][GPT_D], Hb[GPT_T][GPT_D], QKVb[GPT_T][3 * GPT_D], ATTb[GPT_T][GPT_D], Ob[GPT_T][GPT_D], M1b[GPT_T][4 * GPT_D], M2b[GPT_T][GPT_D];
static void forward_batch(const int *toks, int T)
{
   const float scale = 1.0f / sqrtf_((float)HD);
   for (int t = 0; t < T; t++)
      for (int i = 0; i < GPT_D; i++) Xb[t][i] = W(OFF_WTE)[toks[t] * GPT_D + i] + W(OFF_WPE)[(pos + t) * GPT_D + i];
   for (int l = 0; l < GPT_L; l++)
   {
      const int *L = LOFF[l];
      for (int t = 0; t < T; t++) layernorm(Hb[t], Xb[t], W(L[0]), W(L[1]), GPT_D);
      LAYER_GEMM(&QKVb[0][0], 3 * GPT_D, &Hb[0][0], GPT_D, 0, W(L[2]), W(L[3]), T, GPT_D, 3 * GPT_D);
      for (int t = 0; t < T; t++)
         for (int i = 0; i < GPT_D; i++) { Kc[l][pos + t][i] = QKVb[t][GPT_D + i]; Vc[l][pos + t][i] = QKVb[t][2 * GPT_D + i]; }
      for (int t = 0; t < T; t++)
         for (int hh = 0; hh < GPT_H; hh++)
            attend(ATTb[t] + hh * HD, QKVb[t] + hh * HD, &Kc[l][0][hh * HD], &Vc[l][0][hh * HD], pos + t + 1, scale);
      LAYER_GEMM(&Ob[0][0], GPT_D, &ATTb[0][0], GPT_D, 1, W(L[4]), W(L[5]), T, GPT_D, GPT_D);
      for (int t = 0; t < T; t++) for (int i = 0; i < GPT_D; i++) Xb[t][i] += Ob[t][i];
      for (int t = 0; t < T; t++) layernorm(Hb[t], Xb[t], W(L[6]), W(L[7]), GPT_D);
      LAYER_GEMM(&M1b[0][0], 4 * GPT_D, &Hb[0][0], GPT_D, 2, W(L[8]), W(L[9]), T, GPT_D, 4 * GPT_D);
      for (int t = 0; t < T; t++) GELU_INPLACE(M1b[t], 4 * GPT_D);
      LAYER_GEMM(&M2b[0][0], GPT_D, &M1b[0][0], 4 * GPT_D, 3, W(L[10]), W(L[11]), T, 4 * GPT_D, GPT_D);
      for (int t = 0; t < T; t++) for (int i = 0; i < GPT_D; i++) Xb[t][i] += M2b[t][i];
   }
   layernorm(h, Xb[T - 1], W(OFF_LNF_G), W(OFF_LNF_B), GPT_D);     /* only the last token's logits are used */
   matvec(logits, h, W(OFF_HEAD), 0, GPT_D, GPT_V);
   pos += T;
}

/* ---------------------------------------------------------- tokens and the agent loop */
static int stoi[256];
static long n_prefill, n_generated;
static int pending[GPT_T], npending;
static int batches[64], nbatches;

static void feed(const char *s)                 /* prefill: queue text; it runs as one batch before the next generate */
{
   for (; *s; s++) { if (pos + npending >= GPT_T) return; pending[npending++] = stoi[(unsigned char)*s]; }
}
static void flush_prefill(void)
{
   if (!npending) return;
   if (nbatches < 64) batches[nbatches++] = npending;
   forward_batch(pending, npending);
   n_prefill += npending;
   npending = 0;
}
static int ended_nl;                            /* the last generate stopped at a newline (now in the context) */
static int generate(char *dst, int max)         /* greedy until '\n' (kept out of dst) or the window ends */
{
   int n = 0;
   flush_prefill();
   ended_nl = 0;
   while (n < max && pos < GPT_T)
   {
      int best = 0;
      for (int v = 1; v < GPT_V; v++) if (logits[v] > logits[best]) best = v;
      char c = GPT_VOCAB[best];
      n_generated++;
      forward(best);
      if (c == '\n') { ended_nl = 1; break; }
      dst[n++] = c;
   }
   dst[n] = 0;
   return n;
}
static long atol_(const char *s) { long v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }
/* Two knobs for studying sampled simulation (testing/test_pacsim.py's agent
   sweep): PROMPT_REPEAT runs the prompt list that many times, so the run
   gets longer with the same amount of distinct inference; TOOL_WORK gives
   each tool call that many rounds of work of its own and shifts the run's
   balance from the model towards the tools.  A sampler keys on the block
   mix, so a tool that merely hashed different data would still read as one
   repeating loop; instead the rounds are split between four kernels of
   different character (a hash over a table, a byte scan with data-dependent
   branches, insertion sorts, a polynomial in double precision) in
   proportions drawn from the call's operands, so no two calls share a block
   mix, as two different real tools would not.  Neither knob changes an
   answer. */
#ifndef PROMPT_REPEAT
#define PROMPT_REPEAT 1
#endif
#ifndef TOOL_WORK
#define TOOL_WORK 0
#endif
static volatile unsigned long tool_sink;
static volatile double tool_fsink;
static void tool_work(const char *call)
{
#if TOOL_WORK > 0
   static unsigned long table[4096];
   static unsigned char text[8192];
   static int keys[256];
   unsigned long h = 1469598103934665603ul;
   for (const char *c = call; *c; c++) h = (h ^ (unsigned char)*c) * 1099511628211ul;
   /* the split: four weights from the operands' hash, each 1..16 */
   const long w0 = 1 + (long)(h & 15), w1 = 1 + (long)((h >> 4) & 15), w2 = 1 + (long)((h >> 8) & 15), w3 = 1 + (long)((h >> 12) & 15);
   const long wsum = w0 + w1 + w2 + w3;
   const long n0 = TOOL_WORK * w0 / wsum, n1 = TOOL_WORK * w1 / wsum, n2 = TOOL_WORK * w2 / wsum, n3 = TOOL_WORK - n0 - n1 - n2;
   for (long i = 0; i < n0; i++)
   {
      h ^= table[h & 4095]; h *= 1099511628211ul; h = (h << 13) | (h >> 51);
      table[(h >> 7) & 4095] += h;
   }
   unsigned long count = 0;
   for (long i = 0; i < n1; i++)
   {
      unsigned char *t = text + ((h + (unsigned long)i * 131) & 8191);
      if (*t == (unsigned char)h) count++; else if (*t > (unsigned char)(h >> 8)) *t -= 1; else *t += 3;
      h += *t;
   }
   for (long i = 0; i < n2; i += 64)
   {
      for (int k = 0; k < 64; k++) keys[k] = (int)((h >> (k & 31)) ^ (unsigned long)k * 2654435761u);
      for (int k = 1; k < 64; k++) { int v = keys[k], j = k - 1; while (j >= 0 && keys[j] > v) { keys[j + 1] = keys[j]; j--; } keys[j + 1] = v; }
      h += (unsigned long)keys[0] * 31 + (unsigned long)keys[63];
   }
   double x = (double)(h & 0xffff) / 65536.0, acc = 0;
   for (long i = 0; i < n3; i++) { acc = acc * x + 1.0 / (double)(i + 1); x = x * 0.999 + 0.0005; }
   tool_fsink = acc;
   tool_sink = h + count;
#else
   (void)call;
#endif
}

static void run_tool(const char *call, char *res)   /* "add a b", "mul a b", "len w" */
{
   const char *p = call + 4;
   long v = -1;
   tool_work(call);
   if (call[0] == 'a') { long a = atol_(p); while (*p && *p != ' ') p++; v = a + atol_(p + 1); }
   else if (call[0] == 'm') { long a = atol_(p); while (*p && *p != ' ') p++; v = a * atol_(p + 1); }
   else if (call[0] == 'l') { v = 0; while (p[v] && p[v] != ' ') v++; }
   if (v < 0) { res[0] = '?'; res[1] = 0; return; }
   char b[24]; int i = 23; b[i] = 0;
   do { b[--i] = (char)('0' + (v % 10)); v /= 10; } while (v);
   memcpy(res, b + i, (unsigned long)(24 - i));
}

void _start(void)
{
   for (int i = 0; i < 256; i++) stoi[i] = 0;
   for (int i = 0; i < GPT_V; i++) stoi[(unsigned char)GPT_VOCAB[i]] = i;
#ifdef USE_AME
   long ame_status;
   RISCV_AME_ACQUIRE(ame_status, 0L); (void)ame_status;
   RISCV_MSETTYP(0, AME_FP32); RISCV_MSETTYP(1, AME_FP32); RISCV_MSETTYP(2, AME_FP32);
#endif
   out("agent: "); outn(GPT_NPARAM); out(" parameters, d="); outn(GPT_D); out(" L="); outn(GPT_L); out(" ctx="); outn(GPT_T); out(", matvec " MATVEC_NAME
#ifdef USE_W4
       " + w4"
#endif
       "\n");
   int bad = 0;
   char call[64], res[24], ans[64];
   /* The region of interest is the agent loop -- every prompt, tool call and
      answer -- so that run-sniper --roi, and Pac-Sim's sampling of it,
      measure the inference and not the vocabulary table or the unit's
      set-up.  Without --roi the markers change nothing. */
   SimRoiStart();
   for (int r = 0; r < PROMPT_REPEAT; r++)
   for (int i = 0; i < N_PROMPTS; i++)
   {
      pos = 0;
      feed("Q: "); feed(PROMPT_Q[i]); feed("\nT: ");
      generate(call, GEN_CALL);           /* the call and its newline are already in the context ... */
      if (!ended_nl) feed("\n");           /* ... unless the model never produced one (untrained): the numpy protocol adds it */
      run_tool(call, res);
      feed("R: "); feed(res); feed("\nA: ");
      generate(ans, GEN_ANS);
      int as_model = seq(call, PROMPT_CALL_MODEL[i]) && seq(ans, PROMPT_ANS_MODEL[i]);
      int correct = seq(ans, res) && seq(ans, PROMPT_ANS[i]);
      out("Q: "); out(PROMPT_Q[i]); out("\n  T: "); out(call); out("  R: "); out(res); out("  A: "); out(ans);
      out(as_model ? "  [= numpy]" : "  [DIFFERS from numpy]"); out(correct ? " [correct]\n" : " [WRONG]\n");
      if (!as_model) bad |= 1;
      if (!correct) bad |= 2;
   }
   SimRoiEnd();
   out("tokens: "); outn(n_prefill); out(" prefilled, "); outn(n_generated); out(" generated\n");
   out("prefill batches:"); for (int i = 0; i < nbatches; i++) { out(" "); outn(batches[i]); } out("\n");
   out("attention: "); outn(n_att_dots); out(" dots, "); outn(n_att_strips); out(" softmax strips\n");
#ifdef USE_W4
   out("w4: "); outn(n_w4_matvec); out(" int4 matvecs\n");
#endif
#ifdef USE_AME
   RISCV_AME_RELEASE();
#endif
   sys_exit(bad == 0 ? 100 : bad == 1 ? 5 : bad == 2 ? 6 : 7);   /* 6: every line as numpy but the answers wrong -- an untrained model's pass */
}
