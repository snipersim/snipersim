/* A wider AArch64 instruction mix for the decoder: scalar FP and conversions,
 * NEON, integer divide and bit manipulation, load/store pairs, atomics (LSE
 * when built for v8.1+), a jump-table switch and indirect calls.  Freestanding,
 * raw syscalls; prints a checksum that must match the native run. */
typedef unsigned long u64;
static long sys3(long n, long a, long b, long c)
{
   register long x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c, x8 asm("x8") = n;
   asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
   return x0;
}
static void put(const char *s) { long n = 0; while (s[n]) n++; sys3(64, 1, (long)s, n); }
static void putn(u64 v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); put(b + i); }

static float fa[1024], fb[1024];
static double da[512];
static unsigned ua[2048];
static long counter;

static u64 fp_part(void)
{
   double acc = 0;
   for (int i = 0; i < 512; i++)
   {
      da[i] = (double)i * 1.25 + 0.5;
      acc += __builtin_sqrt(da[i]) / (1.0 + da[i] * 0.001);
   }
   float f = 0;
   for (int i = 0; i < 1024; i++) { fa[i] = (float)i * 0.5f; f += fa[i] * 1.5f - (float)(i & 7); }
   return (u64)(acc * 1000.0) + (u64)(long)f;
}

/* Vectorised by the compiler at -O3 (this clang ships no arm_neon.h) */
static u64 neon_part(void)
{
   float sum = 0;
   for (int i = 0; i < 1024; i++)
   {
      fb[i] = fa[i] * 2.0f;
      sum += fa[i] + fb[i] * 0.25f;
   }
   unsigned u = 0;
   for (int i = 0; i < 2048; i++)
      u += ua[i] ^ (ua[i] << 3);
   return (u64)(long)sum + u;
}

static u64 int_part(void)
{
   u64 s = 0;
   for (int i = 0; i < 2048; i++)
   {
      ua[i] = (unsigned)(i * 2654435761u);
      s += ua[i] / (unsigned)(i + 1) + ua[i] % 7u;
      s += (u64)__builtin_clz(ua[i] | 1) + (u64)__builtin_popcount(ua[i]) + (u64)__builtin_ctzll(s | 1);
      s ^= (u64)((long)s * (long)(i + 3)) >> 5;
   }
   return s;
}

static u64 atomic_part(void)
{
   for (int i = 0; i < 1000; i++)
   {
      __atomic_fetch_add(&counter, i, __ATOMIC_SEQ_CST);
      long expect = counter;
      __atomic_compare_exchange_n(&counter, &expect, expect + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
   }
   return (u64)counter;
}

static u64 op0(u64 x) { return x * 3; }
static u64 op1(u64 x) { return x + 17; }
static u64 op2(u64 x) { return x ^ 0x5a5a; }
static u64 (*const ops[])(u64) = { op0, op1, op2 };

static u64 control_part(void)
{
   u64 s = 1;
   for (int i = 0; i < 3000; i++)
   {
      switch (i % 7)
      {
         case 0: s += 1; break;
         case 1: s *= 3; break;
         case 2: s ^= (u64)i; break;
         case 3: s -= 5; break;
         case 4: s = (s << 1) | (s >> 63); break;
         case 5: s += (u64)i * i; break;
         default: s = ops[i % 3](s); break;
      }
   }
   return s;
}

void _start(void)
{
   u64 total = fp_part() + int_part() + neon_part() + atomic_part() + control_part();
   put("a64 stress="); putn(total); put("\n");
   sys3(93, 0, 0, 0);
   for (;;) ;
}
