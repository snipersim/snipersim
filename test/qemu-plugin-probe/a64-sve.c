/* SVE on AArch64 (testing/test_qemu.py, aarch64-sve-simulate): loops the
 * compiler vectorises with SVE -- predicated, vector-length agnostic loads,
 * stores, multiply-adds, a reduction and a gather -- on a guest built with
 * +sve.  QEMU's "max" CPU implements SVE; the trace is simulated on the ARM
 * cores, so Sniper's decoder has to take SVE encodings.  The answer is
 * printed and checked.  Raw Linux system calls, no libc. */
typedef unsigned long u64;
static long sys3(long n, long a, long b, long c)
{
   register long x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c, x8 asm("x8") = n;
   asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
   return x0;
}
static void put(const char *s) { long n = 0; while (s[n]) n++; sys3(64, 1, (long)s, n); }
static void putn(u64 v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); put(b + i); }

#define N 1000
static int a[N], b[N], c[N], idx[N];
static float x[N], y[N];

__attribute__((noinline)) static void axpy(float k, int n) { for (int i = 0; i < n; i++) y[i] += k * x[i]; }
__attribute__((noinline)) static void mla(int n) { for (int i = 0; i < n; i++) c[i] = a[i] * b[i] + c[i]; }
__attribute__((noinline)) static long reduce(int n) { long s = 0; for (int i = 0; i < n; i++) s += a[i] ^ c[i]; return s; }
__attribute__((noinline)) static long gather(int n) { long s = 0; for (int i = 0; i < n; i++) s += b[idx[i]]; return s; }

void _start(void)
{
   for (int i = 0; i < N; i++)
   {
      a[i] = i * 7 + 1; b[i] = (i * 13) % 101; c[i] = i; idx[i] = (i * 37) % N;
      x[i] = (float)(i % 17); y[i] = 1.0f;
   }
   long s = 0;
   for (int r = 0; r < 20; r++)
   {
      axpy(0.5f, N - r);
      mla(N - r);
      s += reduce(N) + gather(N);
   }
   long f = 0;
   for (int i = 0; i < N; i++) f += (long)y[i];
   put("sve sum="); putn((u64)s); put(" f="); putn((u64)f); put("\n");
   sys3(93, 0, 0, 0);
   for (;;) ;
}
