#ifndef __INSTRUCTION_TRACER_H
#define __INSTRUCTION_TRACER_H

#include "fixed_types.h"

class Core;
class DynamicMicroOp;

class InstructionTracer
{
   public:
      static void init();
      static InstructionTracer* create(const Core *core);

      virtual ~InstructionTracer() {}

      virtual void traceInstruction(const DynamicMicroOp *uop, uint64_t cycle_issue, uint64_t cycle_done) = 0;
};

#endif // __INSTRUCTION_TRACER_H
