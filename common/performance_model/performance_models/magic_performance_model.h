#ifndef MAGIC_PERFORMANCE_MODEL_H
#define MAGIC_PERFORMANCE_MODEL_H

#include "performance_model.h"
#include "subsecond_time.h"

class MagicPerformanceModel : public PerformanceModel
{
public:
   MagicPerformanceModel(Core *core);
   ~MagicPerformanceModel();

private:
   bool handleInstruction(Instruction const* instruction);

   bool isModeled(Instruction const* instruction) const;
};

#endif
