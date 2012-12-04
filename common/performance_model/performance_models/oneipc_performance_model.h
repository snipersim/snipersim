#ifndef ONEIPC_PERFORMANCE_MODEL_H
#define ONEIPC_PERFORMANCE_MODEL_H

#include "performance_model.h"

class OneIPCPerformanceModel : public PerformanceModel
{
public:
   OneIPCPerformanceModel(Core *core);
   ~OneIPCPerformanceModel();

private:
   bool handleInstruction(Instruction const* instruction);

   bool isModeled(Instruction const* instruction) const;

   UInt64 m_latency_cutoff;
};

#endif // ONEIPC_PERFORMANCE_MODEL_H
