#ifndef SIMPLE_PERFORMANCE_MODEL_H
#define SIMPLE_PERFORMANCE_MODEL_H

#include "performance_model.h"

class SimplePerformanceModel : public PerformanceModel
{
public:
   SimplePerformanceModel(Core *core);
   ~SimplePerformanceModel();

private:
   bool handleInstruction(Instruction const* instruction);
};

#endif
