/* Bare-metal RISC-V for QEMU's virt board (-bios none -kernel): machine mode
 * from 0x80000000, a loop with branches and memory traffic, the result on
 * the UART (ns16550 at 0x10000000), and the test finisher (0x100000) ends
 * the run: 0x5555 passes, 0x3333 fails. */
typedef unsigned long u64;
#define UART ((volatile unsigned char *)0x10000000UL)
#define FINISHER ((volatile unsigned *)0x100000UL)

static void put(const char *s) { while (*s) *UART = (unsigned char)*s++; }
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

void main_c(void)
{
   u64 s = work(25);
   put("rv sum="); putn(s); put("\n");
   *FINISHER = (s == 3490074579968UL) ? 0x5555 : 0x3333;
   for (;;) ;
}

/* First in RAM: -bios none starts the hart at 0x80000000, not at the ELF
 * entry.  A section lld does not fold into .text, placed there by
 * --section-start=.boot=0x80000000. */
asm(".section .boot,\"ax\",@progbits\n"
    ".globl _start\n"
    "_start:\n"
    "  la sp, rv_stack\n"
    "  li t0, 16384\n"
    "  add sp, sp, t0\n"
    "  j main_c\n"
    ".text\n");
char rv_stack[16384] __attribute__((aligned(16)));
