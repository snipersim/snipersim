#ifndef __INSTRUCTION_TRACER_PRINT_H
#define __INSTRUCTION_TRACER_PRINT_H

#include "instruction_tracer.h"

class InstructionTracerPrint : public InstructionTracer
{
   public:
      InstructionTracerPrint(const Core *core)
         : m_core(core)
      {}
      virtual void traceInstruction(const DynamicMicroOp *uop, uop_times_t *times);

   private:
      const Core *m_core;
};

#endif /* __INSTRUCTION_TRACER_PRINT_H */
