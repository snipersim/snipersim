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
#include "prompts.h"
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
#if defined(USE_IME) || defined(USE_AME) || defined(USE_DMA)
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


/* ---------------------------------------------------------- prefill: the GEMM
 * Y[t][o] = bias[o] + sum_k Xm[t][k] * W[o][k] for T tokens at once (Y and
 * Xm with their own row strides).  The class B shape: the T x nin activation
 * block against the nout x nin weights, so a matrix unit fills every column
 * of its tile and a weight tile is loaded once per group of token tiles
 * instead of once per token. */
#define SEL16(i, OP) do { switch (i) { \
   case 0: OP(0); break; case 1: OP(1); break; case 2: OP(2); break; case 3: OP(3); break; \
   case 4: OP(4); break; case 5: OP(5); break; case 6: OP(6); break; case 7: OP(7); break; \
   case 8: OP(8); break; case 9: OP(9); break; case 10: OP(10); break; case 11: OP(11); break; \
   case 12: OP(12); break; case 13: OP(13); break; case 14: OP(14); break; case 15: OP(15); break; } } while (0)

#if defined(USE_IME)
/* IME_S x IME_S tiles: a token block x an output block; the IME_NACC
 * accumulator groups (v16.. in steps of LMUL) hold that many token tiles, so a
 * weight tile is loaded once per IME_NACC*IME_S tokens.  A batch shorter than
 * a tile is padded: the rows beyond T come from the buffer (GPT_T rows) and
 * are never read back. */
#define IME_ZERO(a)  __asm__ volatile("vmv.v.i v" #a ", 0")
#define IME_MMA(a)   RISCV_VFMMACC_VV(a, 0, 8, 1)
#define IME_STORE(a) IME_VMTS(a, yp, (long)ldy)
#if IME_LMUL == 1
#define SELV(i, OP) do { switch (i) { \
   case 0: OP(16); break; case 1: OP(17); break; case 2: OP(18); break; case 3: OP(19); break; \
   case 4: OP(20); break; case 5: OP(21); break; case 6: OP(22); break; case 7: OP(23); break; \
   case 8: OP(24); break; case 9: OP(25); break; case 10: OP(26); break; case 11: OP(27); break; \
   case 12: OP(28); break; case 13: OP(29); break; case 14: OP(30); break; case 15: OP(31); break; } } while (0)
#elif IME_LMUL == 2
#define SELV(i, OP) do { switch (i) { \
   case 0: OP(16); break; case 1: OP(18); break; case 2: OP(20); break; case 3: OP(22); break; \
   case 4: OP(24); break; case 5: OP(26); break; case 6: OP(28); break; case 7: OP(30); break; } } while (0)
#elif IME_LMUL == 4
#define SELV(i, OP) do { switch (i) { case 0: OP(16); break; case 1: OP(20); break; case 2: OP(24); break; case 3: OP(28); break; } } while (0)
#else
#define SELV(i, OP) do { switch (i) { case 0: OP(16); break; case 1: OP(24); break; } } while (0)
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
         for (int tb = 0; tb < nb; tb++) SELV(tb, IME_ZERO);
         for (int k = 0; k < nin; k += IME_S)
         {
            IME_VMTL(8, W + (long)o * nin + k, (long)nin);           /* W tile: rows o.. */
            for (int tb = 0; tb < nb; tb++)
            {
               IME_VMTL(0, Xm + (long)(g + tb) * IME_S * ldx + k, (long)ldx);   /* X tile: IME_S tokens */
               SELV(tb, IME_MMA);                                               /* C += X x W^T */
            }
         }
         for (int tb = 0; tb < nb; tb++)
         {
            float *yp = Y + (long)(g + tb) * IME_S * ldy + o;
            SELV(tb, IME_STORE);
            if (bias) for (int i = 0; i < IME_S; i++) for (int j = 0; j < IME_S; j++) yp[i * ldy + j] += bias[o + j];
         }
      }
   for (; o < nout; o++)
      for (int t = 0; t < T; t++) Y[(long)t * ldy + o] = (bias ? bias[o] : 0.0f) + dot(Xm + (long)t * ldx, W + (long)o * nin, nin);
}
#elif defined(USE_AME)
/* N x N squares: a token block x an output block; the 16 accumulators hold 16
 * token squares (16*N tokens), so a weight square is loaded once per prefill.
 * A batch shorter than a square is padded: the rows beyond T come from the
 * buffer (GPT_T rows) and are never read back. */
#define AME_SET(a)  RISCV_ASETTYP(a, AME_FP32)
#define AME_MMA(a)  RISCV_MMULBTACC_2D(a, 0, 1)
#define AME_MOV(a)  RISCV_MMOV_M_A(2, a)
static void gemm(float *Y, int ldy, const float *Xm, int ldx, const float *W, const float *bias, int T, int nin, int nout)
{
   const int TB = (T + AME_N - 1) / AME_N;
   int o = 0;
   for (; o + AME_N <= nout; o += AME_N)
      for (int g = 0; g < TB; g += 16)
      {
         const int nb = TB - g < 16 ? TB - g : 16;
         for (int tb = 0; tb < nb; tb++) SEL16(tb, AME_SET);                      /* zero the accumulators */
         for (int k = 0; k < nin; k += AME_N)
         {
            RISCV_MLS_ST(1, W + (long)o * nin + k, (long)nin * 4);                 /* W square: rows o.. */
            for (int tb = 0; tb < nb; tb++)
            {
               RISCV_MLS_ST(0, Xm + (long)(g + tb) * AME_N * ldx + k, (long)ldx * 4);  /* X square: N tokens */
               SEL16(tb, AME_MMA);                                                 /* acc += X x W^T */
            }
         }
         for (int tb = 0; tb < nb; tb++)
         {
            float *yp = Y + (long)(g + tb) * AME_N * ldy + o;
            SEL16(tb, AME_MOV);
            RISCV_MSS_ST(2, yp, (long)ldy * 4);
            if (bias) for (int i = 0; i < AME_N; i++) for (int j = 0; j < AME_N; j++) yp[i * ldy + j] += bias[o + j];
         }
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

/* ---------------------------------------------------------- the model */
#define HD (GPT_D / GPT_H)
static float Kc[GPT_L][GPT_T][GPT_D], Vc[GPT_L][GPT_T][GPT_D];   /* KV cache */
static float x[GPT_D], h[GPT_D], qkv[3 * GPT_D], att[GPT_D], o[GPT_D], m1[4 * GPT_D], m2[GPT_D], logits[GPT_V];
static float scores[GPT_T];
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

/* one token in at position pos, logits out */
static void forward(int tok)
{
   for (int i = 0; i < GPT_D; i++) x[i] = W(OFF_WTE)[tok * GPT_D + i] + W(OFF_WPE)[pos * GPT_D + i];
   const float scale = 1.0f / sqrtf_((float)HD);
   for (int l = 0; l < GPT_L; l++)
   {
      const int *L = LOFF[l];
      layernorm(h, x, W(L[0]), W(L[1]), GPT_D);
      matvec(qkv, h, W(L[2]), W(L[3]), GPT_D, 3 * GPT_D);
      for (int i = 0; i < GPT_D; i++) { Kc[l][pos][i] = qkv[GPT_D + i]; Vc[l][pos][i] = qkv[2 * GPT_D + i]; }
      for (int hh = 0; hh < GPT_H; hh++)
      {
         const float *q = qkv + hh * HD;
         float mx = -1e30f;
         for (int t = 0; t <= pos; t++) { scores[t] = dot(q, &Kc[l][t][hh * HD], HD) * scale; if (scores[t] > mx) mx = scores[t]; }
         float sum = 0;
         for (int t = 0; t <= pos; t++) { scores[t] = expf_(scores[t] - mx); sum += scores[t]; }
         for (int i = 0; i < HD; i++) att[hh * HD + i] = 0;
         for (int t = 0; t <= pos; t++) { float p = scores[t] / sum; for (int i = 0; i < HD; i++) att[hh * HD + i] += p * Vc[l][t][hh * HD + i]; }
      }
      matvec(o, att, W(L[4]), W(L[5]), GPT_D, GPT_D);
      for (int i = 0; i < GPT_D; i++) x[i] += o[i];
      layernorm(h, x, W(L[6]), W(L[7]), GPT_D);
      matvec(m1, h, W(L[8]), W(L[9]), GPT_D, 4 * GPT_D);
      for (int i = 0; i < 4 * GPT_D; i++) m1[i] = gelu(m1[i]);
      matvec(m2, m1, W(L[10]), W(L[11]), 4 * GPT_D, GPT_D);
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
      gemm(&QKVb[0][0], 3 * GPT_D, &Hb[0][0], GPT_D, W(L[2]), W(L[3]), T, GPT_D, 3 * GPT_D);
      for (int t = 0; t < T; t++)
         for (int i = 0; i < GPT_D; i++) { Kc[l][pos + t][i] = QKVb[t][GPT_D + i]; Vc[l][pos + t][i] = QKVb[t][2 * GPT_D + i]; }
      for (int t = 0; t < T; t++)
         for (int hh = 0; hh < GPT_H; hh++)
         {
            const float *q = QKVb[t] + hh * HD;
            float mx = -1e30f;
            for (int u = 0; u <= pos + t; u++) { scores[u] = dot(q, &Kc[l][u][hh * HD], HD) * scale; if (scores[u] > mx) mx = scores[u]; }
            float sum = 0;
            for (int u = 0; u <= pos + t; u++) { scores[u] = expf_(scores[u] - mx); sum += scores[u]; }
            for (int i = 0; i < HD; i++) ATTb[t][hh * HD + i] = 0;
            for (int u = 0; u <= pos + t; u++) { float pr = scores[u] / sum; for (int i = 0; i < HD; i++) ATTb[t][hh * HD + i] += pr * Vc[l][u][hh * HD + i]; }
         }
      gemm(&Ob[0][0], GPT_D, &ATTb[0][0], GPT_D, W(L[4]), W(L[5]), T, GPT_D, GPT_D);
      for (int t = 0; t < T; t++) for (int i = 0; i < GPT_D; i++) Xb[t][i] += Ob[t][i];
      for (int t = 0; t < T; t++) layernorm(Hb[t], Xb[t], W(L[6]), W(L[7]), GPT_D);
      gemm(&M1b[0][0], 4 * GPT_D, &Hb[0][0], GPT_D, W(L[8]), W(L[9]), T, GPT_D, 4 * GPT_D);
      for (int t = 0; t < T; t++) for (int i = 0; i < 4 * GPT_D; i++) M1b[t][i] = gelu(M1b[t][i]);
      gemm(&M2b[0][0], GPT_D, &M1b[0][0], 4 * GPT_D, W(L[10]), W(L[11]), T, 4 * GPT_D, GPT_D);
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
static void run_tool(const char *call, char *res)   /* "add a b", "mul a b", "len w" */
{
   const char *p = call + 4;
   long v = -1;
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
   out("agent: "); outn(GPT_NPARAM); out(" parameters, d="); outn(GPT_D); out(" L="); outn(GPT_L); out(" ctx="); outn(GPT_T); out(", matvec " MATVEC_NAME "\n");
   int bad = 0;
   char call[64], res[24], ans[64];
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
   out("tokens: "); outn(n_prefill); out(" prefilled, "); outn(n_generated); out(" generated\n");
   out("prefill batches:"); for (int i = 0; i < nbatches; i++) { out(" "); outn(batches[i]); } out("\n");
#ifdef USE_AME
   RISCV_AME_RELEASE();
#endif
   sys_exit(bad == 0 ? 100 : bad == 1 ? 5 : bad == 2 ? 6 : 7);   /* 6: every line as numpy but the answers wrong -- an untrained model's pass */
}
