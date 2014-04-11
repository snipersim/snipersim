#ifndef INSTRUCTION_MODELING_H
#define INSTRUCTION_MODELING_H

#include "fixed_types.h"
#include "inst_mode.h"
#include <pin.H>

class Instruction;

class InstructionModeling
{
   public:
      static VOID addInstructionModeling(TRACE trace, INS ins, InstMode::inst_mode_t inst_mode);
      static Instruction* decodeInstruction(INS ins);
      static void handleInstruction(THREADID thread_id, Instruction *instruction);
      static void handleBasicBlock(THREADID thread_id);
      static VOID countInstructions(THREADID threadid, ADDRINT address, INT32 count);
      static VOID accessInstructionCacheWarmup(THREADID threadid, ADDRINT address, UINT32 size);
};

#endif
