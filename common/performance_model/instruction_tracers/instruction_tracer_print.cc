#include "instruction_tracer_print.h"
#include "core.h"
#include "dynamic_micro_op.h"

extern "C" {
#include <xed-iclass-enum.h>
}

void InstructionTracerPrint::traceInstruction(const DynamicMicroOp *uop, uint64_t cycle_issue, uint64_t cycle_done)
{
   std::cout << "[INS_PRINT:" << m_core->getId() << "] " << xed_iclass_enum_t2str(uop->getMicroOp()->getInstructionOpcode()) << std::endl;;
}
