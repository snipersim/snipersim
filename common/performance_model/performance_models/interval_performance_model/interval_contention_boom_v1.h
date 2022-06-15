/*
 * This file is covered under the Interval Academic License, see LICENCE.academic
 */

#ifndef __INTERVAL_CONTENTION_BOOM_V1_H
#define __INTERVAL_CONTENTION_BOOM_V1_H

#include "interval_contention.h"
#include "dynamic_micro_op_boom_v1.h"

class IntervalContentionBoomV1 : public IntervalContention {
   private:
      const CoreModel *m_core_model;

      uint64_t m_count_byport[8 /*MicroOp::UOP_PORT_SIZE*/];
      uint64_t m_cpContrByPort[DynamicMicroOpBoomV1::UOP_PORT_SIZE];

   public:
      IntervalContentionBoomV1(const Core *core, const CoreModel *core_model);

      virtual void clearFunctionalUnitStats();
      virtual void addFunctionalUnitStats(const DynamicMicroOp *uop);
      virtual void removeFunctionalUnitStats(const DynamicMicroOp *uop);
      virtual uint64_t getEffectiveCriticalPathLength(uint64_t critical_path_length, bool update_reason);
};

#endif // __INTERVAL_CONTENTION_BOOM_V1_H
