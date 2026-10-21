/* Code rewritten in place on AArch64 (testing/test_qemu.py,
 * aarch64-smc-rewrite).
 *
 * A fresh page gets code A ("mov w0, #1; ret"), which runs; then code B
 * ("mov w0, #2; ret") at the same address, which runs; then A again.  The
 * trace must carry each run's own bytes at that address.  Between a write
 * and running it, the cache maintenance the architecture requires: clean the
 * data cache to the point of unification, invalidate the instruction cache,
 * and synchronise.  Freestanding: raw Linux system calls, no libc. */
typedef unsigned long u64;
static long sys(long n, long a, long b, long c, long d, long e, long f)
{
   register long x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c, x3 asm("x3") = d,
                 x4 asm("x4") = e, x5 asm("x5") = f, x8 asm("x8") = n;
   asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
   return x0;
}
static void put(const char *s) { long n = 0; while (s[n]) n++; sys(64, 1, (long)s, n, 0, 0, 0); }
static void putn(u64 v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); put(b + i); }
static void puthex(u64 v) { char b[24]; int i = 23; b[i] = 0; do { int d = v & 15; b[--i] = d < 10 ? '0' + d : 'a' + d - 10; v >>= 4; } while (v); put(b + i); }

static void run(const char *tag, unsigned *page, long expected)
{
   asm volatile("dc cvau, %0\n\tdsb ish\n\tic ivau, %0\n\tdsb ish\n\tisb" :: "r"(page) : "memory");
   long r = ((long (*)(void))(void *)page)();
   put(tag); put(" pc=0x"); puthex((u64)page); put(" returned="); putn(r);
   put(" expected="); putn(expected); put("\n");
}

void _start(void)
{
   unsigned *page = (unsigned *)sys(222, 0, 4096, 7 /* RWX */, 0x22 /* PRIVATE|ANON */, -1, 0);
   page[0] = 0x52800020; page[1] = 0xd65f03c0;                    /* mov w0, #1; ret */
   run("A first", page, 1);
   page[0] = 0x52800040;                                          /* mov w0, #2 */
   run("B rewritten", page, 2);
   page[0] = 0x52800020;
   run("A again", page, 1);
   sys(93, 0, 0, 0, 0, 0, 0);
   for (;;) ;
}
