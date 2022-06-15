/*
 * This file is covered under the Interval Academic License, see LICENCE.academic
 */

#ifndef __ROB_CONTENTION_BOOM_V1_H
#define __ROB_CONTENTION_BOOM_V1_H

#include "rob_contention.h"
#include "contention_model.h"
#include "core_model_boom_v1.h"
#include "dynamic_micro_op_boom_v1.h"

#include <vector>

class RobContentionBoomV1 : public RobContention {
   private:
      const CoreModel *m_core_model;
      uint64_t m_cache_block_mask;
      ComponentTime m_now;

      // port contention
      bool ports[DynamicMicroOpBoomV1::UOP_PORT_SIZE];
      int ports_generic012;

      std::vector<SubsecondTime> alu_used_until;

   public:
      RobContentionBoomV1(const Core *core, const CoreModel *core_model);

      void initCycle(SubsecondTime now);
      bool tryIssue(const DynamicMicroOp &uop);
      bool noMore();
      void doIssue(DynamicMicroOp &uop);
};

#endif // __ROB_CONTENTION_BOOM_V1_H
