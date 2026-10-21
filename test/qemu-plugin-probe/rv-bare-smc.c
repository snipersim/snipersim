/* Code rewritten in place on a bare-metal RISC-V machine (QEMU's virt board,
 * -bios none -kernel), for testing/test_qemu.py, riscv-system-smc-rewrite.
 *
 * As rv-smc-rewrite.c, in machine mode: code A ("addi a0, x0, 1; ret") is
 * written to a buffer and run, then compressed code B ("c.li a0, 2; c.jr
 * ra") at the same address, then A again; fence.i between, as the ISA
 * requires.  The results go to the UART, and the test finisher ends the run:
 * 0x5555 when they were 1, 2 and 1. */
typedef unsigned long u64;
#define UART ((volatile unsigned char *)0x10000000UL)
#define FINISHER ((volatile unsigned *)0x100000UL)

static void put(const char *s) { while (*s) *UART = (unsigned char)*s++; }
static void putn(u64 v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); put(b + i); }
static void puthex(u64 v) { char b[24]; int i = 23; b[i] = 0; do { int d = v & 15; b[--i] = d < 10 ? '0' + d : 'a' + d - 10; v >>= 4; } while (v); put(b + i); }

static unsigned slot[1024] __attribute__((aligned(4096)));

static long run(const char *tag, long expected)
{
   asm volatile("fence.i" ::: "memory");
   long r = ((long (*)(void))(void *)slot)();
   put(tag); put(" pc=0x"); puthex((u64)slot); put(" returned="); putn(r);
   put(" expected="); putn(expected); put("\n");
   return r == expected;
}

void main_c(void)
{
   long ok = 1;
   slot[0] = 0x00100513; slot[1] = 0x00008067;                    /* addi a0, x0, 1; ret */
   ok &= run("A first", 1);
   ((volatile unsigned short *)slot)[0] = 0x4509;                 /* c.li a0, 2 */
   ((volatile unsigned short *)slot)[1] = 0x8082;                 /* c.jr ra */
   ok &= run("B rewritten", 2);
   slot[0] = 0x00100513; slot[1] = 0x00008067;
   ok &= run("A again", 1);
   *FINISHER = ok ? 0x5555 : 0x3333;
   for (;;) ;
}

asm(".section .boot,\"ax\",@progbits\n"
    ".globl _start\n"
    "_start:\n"
    "  la sp, rv_stack\n"
    "  li t0, 16384\n"
    "  add sp, sp, t0\n"
    "  j main_c\n"
    ".text\n");
char rv_stack[16384] __attribute__((aligned(16)));
