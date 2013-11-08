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
      InstructionTracerFPStats(const Core *core);
      static void init();
      virtual void traceInstruction(const DynamicMicroOp *uop, uint64_t cycle_issue, uint64_t cycle_done);

   private:
      const Core *m_core;
      std::unordered_map<int, uint64_t> m_iclasses;
};

#endif /* __INSTRUCTION_TRACER_FPSTATS_H */
