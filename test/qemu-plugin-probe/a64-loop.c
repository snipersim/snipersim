/* A freestanding AArch64 guest for the QEMU front end: a loop with
 * conditional branches and memory traffic, its result printed and returned.
 * Raw Linux system calls (write 64, exit 93), no libc. */
typedef unsigned long u64;
static long sys3(long n, long a, long b, long c)
{
   register long x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c, x8 asm("x8") = n;
   asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
   return x0;
}
static void put(const char *s) { long n = 0; while (s[n]) n++; sys3(64, 1, (long)s, n); }
static void putn(u64 v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); put(b + i); }

static unsigned buf[4096];

static u64 work(int n)
{
   u64 sum = 0;
   for (int r = 0; r < n; r++)
      for (int i = 0; i < 4096; i++)
      {
         buf[i] = buf[i] * 3 + (unsigned)i;
         if (buf[i] & 1)
            sum += buf[i] >> 3;
         else
            sum ^= buf[i];
      }
   return sum;
}

void _start(void)
{
   u64 s = work(25);
   put("a64 sum="); putn(s); put("\n");
   sys3(93, (long)(s & 0x7f), 0, 0);
   for (;;) ;
}
