#include "instruction_tracer_print.h"
#include "instruction.h"
#include "micro_op.h"

#include <iostream>
#include <vector>

extern "C" {
#include <xed-iclass-enum.h>
}

void InstructionTracerPrint::handleInstruction(Instruction const* instruction)
{
   for (std::vector<const MicroOp *>::const_iterator uop = instruction->getMicroOps()->begin() ; uop != instruction->getMicroOps()->end() ; ++uop)
   {
      std::cout << "[INS_PRINT:" << m_id << "] " << xed_iclass_enum_t2str((*uop)->getInstructionOpcode()) << std::endl;;
      break;
   }
}
