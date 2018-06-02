#ifndef __CORE_MODEL_BOOM_V1_H
#define __CORE_MODEL_BOOM_V1_H

#include "core_model.h"
#include "dynamic_micro_op_boom_v1.h"

class CoreModelBoomV1 : public BaseCoreModel<DynamicMicroOpBoomV1>
{
   private:
      unsigned int m_lll_cutoff;

   public:
      CoreModelBoomV1();

      virtual IntervalContention* createIntervalContentionModel(const Core *core) const;
      virtual RobContention* createRobContentionModel(const Core *core) const;

      virtual DynamicMicroOp* createDynamicMicroOp(Allocator *alloc, const MicroOp *uop, ComponentPeriod period) const;

      virtual unsigned int getInstructionLatency(const MicroOp *uop) const;
      virtual unsigned int getAluLatency(const MicroOp *uop) const;
      virtual unsigned int getBypassLatency(const DynamicMicroOp *uop) const;
      virtual unsigned int getLongestLatency() const;
      virtual unsigned int getLongLatencyCutoff() const { return m_lll_cutoff; }
};

#endif // __CORE_MODEL_BOOM_V1_H
