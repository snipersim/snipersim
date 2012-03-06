#ifndef INSTRUCTIONLATENCIES_HPP_
#define INSTRUCTIONLATENCIES_HPP_

#include "micro_op.h"

extern "C"
{
   #include <xed-iclass-enum.h>
}

void initilizeInstructionLatencies();
unsigned int getInstructionLatency(xed_iclass_enum_t instruction_type);
unsigned int getAluLatency(MicroOp &uop);
unsigned int getBypassLatency(MicroOp::uop_bypass_t bypass_type);

unsigned int getLongestLatency();

#endif /* INSTRUCTIONLATENCIES_HPP_ */
