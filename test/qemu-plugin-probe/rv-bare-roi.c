/* rv-bare.c with a region of interest: five rounds of work before
 * SimRoiStart(), ten inside, five after.  Full-system QEMU has no response
 * channel, so the simulator acts on the markers without answering them and
 * SimInSimulator() reads 0 here, as it does on hardware. */
#include "sim_api.h"

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
   u64 s = work(5);
   SimRoiStart();
   s += work(10);
   SimRoiEnd();
   s += work(5);
   put("rv roi sum="); putn(s); put("\n");
   *FINISHER = (s == 2152578694144UL) ? 0x5555 : 0x3333;
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
