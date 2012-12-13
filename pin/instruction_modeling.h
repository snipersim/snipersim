#ifndef INSTRUCTION_MODELING_H
#define INSTRUCTION_MODELING_H

#include "fixed_types.h"
#include "inst_mode.h"
#include <pin.H>

class BasicBlock;

class InstructionModeling
{
   public:
      static BOOL addInstructionModeling(TRACE trace, INS ins, BasicBlock *basic_block, InstMode::inst_mode_t inst_mode);
      static void handleBasicBlock(THREADID thread_id, BasicBlock *sim_basic_block);
      static VOID countInstructions(THREADID threadid, ADDRINT address, INT32 count);
      static VOID accessInstructionCacheWarmup(THREADID threadid, ADDRINT address, UINT32 size);
};

#endif
