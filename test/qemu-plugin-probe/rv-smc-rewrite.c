/* Code rewritten in place on RISC-V (testing/test_qemu.py, riscv-smc-rewrite).
 *
 * A fresh page gets code A ("addi a0, x0, 1; ret", two 4-byte instructions),
 * which runs; then code B, compressed ("c.li a0, 2; c.jr ra", two 2-byte
 * instructions) at the same address, which runs; then A again.  The trace
 * must carry each run's own bytes at that address.  This is the shape of the
 * report that found the problem ("SIFT Reader fails to change the instruction
 * it has associated with the PC"), with the instruction length changing too.
 * Freestanding: raw Linux system calls, no libc. */
typedef unsigned long u64;
static long sys(long n, long a, long b, long c, long d, long e, long f)
{
   register long a0 asm("a0") = a, a1 asm("a1") = b, a2 asm("a2") = c, a3 asm("a3") = d,
                 a4 asm("a4") = e, a5 asm("a5") = f, a7 asm("a7") = n;
   asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7) : "memory");
   return a0;
}
static void put(const char *s) { long n = 0; while (s[n]) n++; sys(64, 1, (long)s, n, 0, 0, 0); }
static void putn(u64 v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); put(b + i); }
static void puthex(u64 v) { char b[24]; int i = 23; b[i] = 0; do { int d = v & 15; b[--i] = d < 10 ? '0' + d : 'a' + d - 10; v >>= 4; } while (v); put(b + i); }

static void run(const char *tag, void *page, long expected)
{
   asm volatile("fence.i" ::: "memory");
   long r = ((long (*)(void))page)();
   put(tag); put(" pc=0x"); puthex((u64)page); put(" returned="); putn(r);
   put(" expected="); putn(expected); put("\n");
}

void _start(void)
{
   unsigned *page = (unsigned *)sys(222, 0, 4096, 7 /* RWX */, 0x22 /* PRIVATE|ANON */, -1, 0);
   page[0] = 0x00100513; page[1] = 0x00008067;                    /* addi a0, x0, 1; ret */
   run("A first", page, 1);
   ((unsigned short *)page)[0] = 0x4509;                          /* c.li a0, 2 */
   ((unsigned short *)page)[1] = 0x8082;                          /* c.jr ra */
   run("B rewritten", page, 2);
   page[0] = 0x00100513; page[1] = 0x00008067;
   run("A again", page, 1);
   sys(93, 0, 0, 0, 0, 0, 0);
}
