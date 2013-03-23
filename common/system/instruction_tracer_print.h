#ifndef __INSTRUCTION_TRACER_PRINT_H
#define __INSTRUCTION_TRACER_PRINT_H

#include "instruction_tracer.h"

class InstructionTracerPrint : public InstructionTracer
{
   public:
      InstructionTracerPrint(int core_id)
         : m_id(core_id)
      {}
      static void init() {}
      virtual void handleInstruction(Instruction const* instruction);

   private:
      int m_id;
};

#endif /* __INSTRUCTION_TRACER_PRINT_H */
