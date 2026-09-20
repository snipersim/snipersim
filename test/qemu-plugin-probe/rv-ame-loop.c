/* The cost of one plugin-emulated instruction: LOOPS ame.release
 * instructions in a row (the cheapest AME instruction for the plugin's
 * model: no register or memory access, it clears ownership), so a timed
 * run with the plugin's claim on and off measures the mechanism, not the
 * model.  Exits 100. */
#include "riscv_insn.h"
#ifndef LOOPS
#define LOOPS 4000000
#endif
char stack_area[8192] __attribute__((aligned(16)));
__asm__(".globl _start\n_start:\n\tlla sp, stack_area + 8192\n\tcall main\n\tli a7, 93\n\tecall\n");
int main(void)
{
   for (long i = 0; i < LOOPS / 8; i++)
   {
      RISCV_AME_RELEASE(); RISCV_AME_RELEASE(); RISCV_AME_RELEASE(); RISCV_AME_RELEASE();
      RISCV_AME_RELEASE(); RISCV_AME_RELEASE(); RISCV_AME_RELEASE(); RISCV_AME_RELEASE();
   }
   return 100;
}
