#ifndef __CORE_MODEL_CORTEX_A72_H
#define __CORE_MODEL_CORTEX_A72_H

#include "core_model.h"
#include "dynamic_micro_op_cortex_a72.h"

class CoreModelCortexA72 : public BaseCoreModel<DynamicMicroOpCortexA72>
{
   private:
      unsigned int m_lll_cutoff;

   public:
      CoreModelCortexA72();

      virtual IntervalContention* createIntervalContentionModel(const Core *core) const;
      virtual RobContention* createRobContentionModel(const Core *core) const;

      virtual DynamicMicroOp* createDynamicMicroOp(Allocator *alloc, const MicroOp *uop, ComponentPeriod period) const;

      virtual unsigned int getInstructionLatency(const MicroOp *uop) const;
      virtual unsigned int getAluLatency(const MicroOp *uop) const;
      virtual unsigned int getBypassLatency(const DynamicMicroOp *uop) const;
      virtual unsigned int getLongestLatency() const;
      virtual unsigned int getLongLatencyCutoff() const { return m_lll_cutoff; }
};

#endif // __CORE_MODEL_CORTEX_A72_H
