#include "instruction_tracer_print.h"
#include "core.h"
#include "dynamic_micro_op.h"

extern "C" {
#include <xed-iclass-enum.h>
}

void InstructionTracerPrint::traceInstruction(const DynamicMicroOp *uop, uop_times_t *times)
{
   std::cout << "[INS_PRINT:" << m_core->getId() << "] " << xed_iclass_enum_t2str(uop->getMicroOp()->getInstructionOpcode()) << std::endl;;
}
