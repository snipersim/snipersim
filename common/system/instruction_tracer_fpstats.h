#ifndef __INSTRUCTION_TRACER_FPSTATS_H
#define __INSTRUCTION_TRACER_FPSTATS_H

#include "instruction_tracer.h"

extern "C" {
#include <xed-iclass-enum.h>
}

#include <unordered_map>

class InstructionTracerFPStats : public InstructionTracer
{
   public:
      InstructionTracerFPStats(int core_id);
      static void init();
      virtual void handleInstruction(Instruction const* instruction);

   private:
      int m_id;
      std::unordered_map<int, uint64_t> m_iclasses;
};

#endif /* __INSTRUCTION_TRACER_FPSTATS_H */
