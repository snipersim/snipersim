#include <assert.h>

#include "instruction_latencies.h"

static unsigned int instructionLatencies[XED_ICLASS_LAST];
static unsigned int bypassLatencies[MicroOp::UOP_BYPASS_SIZE];

// Intel Nehalem Latencies
// http://www.agner.org/optimize

void initilizeInstructionLatencies()
{
   // Default instruction latency is one cycle
   for (unsigned int i = 0 ; i < XED_ICLASS_LAST ; i++)
   {
      instructionLatencies[i] = 1;
   }

   instructionLatencies[XED_ICLASS_MUL] = 3;
   instructionLatencies[XED_ICLASS_IMUL] = 3;

   instructionLatencies[XED_ICLASS_DIV] = 28; // 32-bit: 17-28, 64-bit: 28-90
   instructionLatencies[XED_ICLASS_IDIV] = 28; // 32-bit: 17-28, 64-bit: 28-90


   instructionLatencies[XED_ICLASS_ADDPS] = 3;
   instructionLatencies[XED_ICLASS_ADDSS] = 3;
   instructionLatencies[XED_ICLASS_ADDSUBPS] = 3;
   instructionLatencies[XED_ICLASS_SUBPS] = 3;
   instructionLatencies[XED_ICLASS_SUBSS] = 3;
   instructionLatencies[XED_ICLASS_VADDPS] = 3;
   instructionLatencies[XED_ICLASS_VADDSS] = 3;
   instructionLatencies[XED_ICLASS_VADDSUBPS] = 3;
   instructionLatencies[XED_ICLASS_VSUBPS] = 3;
   instructionLatencies[XED_ICLASS_VSUBSS] = 3;

   instructionLatencies[XED_ICLASS_ADDPD] = 3;
   instructionLatencies[XED_ICLASS_ADDSD] = 3;
   instructionLatencies[XED_ICLASS_ADDSUBPD] = 3;
   instructionLatencies[XED_ICLASS_SUBPD] = 3;
   instructionLatencies[XED_ICLASS_SUBSD] = 3;
   instructionLatencies[XED_ICLASS_VADDPD] = 3;
   instructionLatencies[XED_ICLASS_VADDSD] = 3;
   instructionLatencies[XED_ICLASS_VADDSUBPD] = 3;
   instructionLatencies[XED_ICLASS_VSUBPD] = 3;
   instructionLatencies[XED_ICLASS_VSUBSD] = 3;


   instructionLatencies[XED_ICLASS_MULSS] = 4;
   instructionLatencies[XED_ICLASS_MULPS] = 4;
   instructionLatencies[XED_ICLASS_VMULSS] = 4;
   instructionLatencies[XED_ICLASS_VMULPS] = 4;

   instructionLatencies[XED_ICLASS_MULSD] = 5;
   instructionLatencies[XED_ICLASS_MULPD] = 5;
   instructionLatencies[XED_ICLASS_VMULSD] = 5;
   instructionLatencies[XED_ICLASS_VMULPD] = 5;

   instructionLatencies[XED_ICLASS_DIVSS] = 13;
   instructionLatencies[XED_ICLASS_DIVPS] = 13;
   instructionLatencies[XED_ICLASS_VDIVSS] = 13;
   instructionLatencies[XED_ICLASS_VDIVPS] = 13;

   instructionLatencies[XED_ICLASS_DIVSD] = 21;
   instructionLatencies[XED_ICLASS_DIVPD] = 21;
   instructionLatencies[XED_ICLASS_VDIVSD] = 21;
   instructionLatencies[XED_ICLASS_VDIVPD] = 21;


   /* bypass latencies */
   /* http://www.agner.org/optimize/microarchitecture.pdf page 86-87 */

   bypassLatencies[MicroOp::UOP_BYPASS_NONE] = 0;
   bypassLatencies[MicroOp::UOP_BYPASS_LOAD_FP] = 2;
   bypassLatencies[MicroOp::UOP_BYPASS_FP_STORE] = 1;
}

unsigned int getInstructionLatency(xed_iclass_enum_t instruction_type) {
   assert(instruction_type >= 0 && instruction_type < XED_ICLASS_LAST);
   return instructionLatencies[instruction_type];
}

unsigned int getAluLatency(MicroOp &uop) {
   switch(uop.getInstructionOpcode()) {
      case XED_ICLASS_DIV:
      case XED_ICLASS_IDIV:
         if (uop.getOperandSize() > 32)
            return 28; // Approximate, data-dependent
         else
            return 9;  // Approximate, data-dependent
      default:
         LOG_PRINT_ERROR("Don't know the ALU latency for this MicroOp.");
   }
}

unsigned int getBypassLatency(MicroOp::uop_bypass_t bypass_type) {
   assert(bypass_type >=0 && bypass_type < MicroOp::UOP_BYPASS_SIZE);
   return bypassLatencies[bypass_type];
}

unsigned int getLongestLatency()
{
   return 60;
}
