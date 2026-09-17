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
 * vfredusum.vs at the end), the first optimization lever. */

#include <stdint.h>
#include "model_weights.h"
#include "prompts.h"

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
static void matvec(float *y, const float *x, const float *W, const float *bias, int nin, int nout)
{
   for (int o = 0; o < nout; o++)
      y[o] = (bias ? bias[o] : 0.0f) + dot(x, W + (long)o * nin, nin);
}
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

/* ---------------------------------------------------------- the model */
#define HD (GPT_D / GPT_H)
static float Kc[GPT_L][GPT_T][GPT_D], Vc[GPT_L][GPT_T][GPT_D];   /* KV cache */
static float x[GPT_D], h[GPT_D], qkv[3 * GPT_D], att[GPT_D], o[GPT_D], m1[4 * GPT_D], m2[GPT_D], logits[GPT_V];
static float scores[GPT_T];
static int pos;

#define W(off) (GPT_W + (off))
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

/* ---------------------------------------------------------- tokens and the agent loop */
static int stoi[256];
static long n_prefill, n_generated;

static void feed(const char *s)                 /* prefill: consume text, discard logits */
{
   for (; *s; s++) { if (pos >= GPT_T) return; forward(stoi[(unsigned char)*s]); n_prefill++; }
}
static int generate(char *dst, int max)         /* greedy until '\n' (kept out of dst) or the window ends */
{
   int n = 0;
   while (n < max && pos < GPT_T)
   {
      int best = 0;
      for (int v = 1; v < GPT_V; v++) if (logits[v] > logits[best]) best = v;
      char c = GPT_VOCAB[best];
      n_generated++;
      forward(best);
      if (c == '\n') break;
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
   out("agent: "); outn(GPT_NPARAM); out(" parameters, d="); outn(GPT_D); out(" L="); outn(GPT_L); out(" ctx="); outn(GPT_T); out(", matvec " MATVEC_NAME "\n");
   int bad = 0;
   char call[64], res[24], ans[64];
   for (int i = 0; i < N_PROMPTS; i++)
   {
      pos = 0;
      feed("Q: "); feed(PROMPT_Q[i]); feed("\nT: ");
      generate(call, 40);                 /* the call and its newline are already in the context */
      run_tool(call, res);
      feed("R: "); feed(res); feed("\nA: ");
      generate(ans, 24);
      int as_model = seq(call, PROMPT_CALL_MODEL[i]) && seq(ans, PROMPT_ANS_MODEL[i]);
      int correct = seq(ans, res) && seq(ans, PROMPT_ANS[i]);
      out("Q: "); out(PROMPT_Q[i]); out("\n  T: "); out(call); out("  R: "); out(res); out("  A: "); out(ans);
      out(as_model ? "  [= numpy]" : "  [DIFFERS from numpy]"); out(correct ? " [correct]\n" : " [WRONG]\n");
      if (!as_model) bad |= 1;
      if (!correct) bad |= 2;
   }
   out("tokens: "); outn(n_prefill); out(" prefilled, "); outn(n_generated); out(" generated\n");
   sys_exit(bad == 0 ? 100 : bad == 1 ? 5 : bad == 2 ? 6 : 7);
}
