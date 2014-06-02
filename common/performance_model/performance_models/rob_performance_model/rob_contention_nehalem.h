/*
 * This file is covered under the Interval Academic License, see LICENCE.interval
 */

#ifndef __ROB_CONTENTION_NEHALEM_H
#define __ROB_CONTENTION_NEHALEM_H

#include "rob_contention.h"
#include "contention_model.h"
#include "core_model_nehalem.h"
#include "dynamic_micro_op_nehalem.h"

#include <vector>

class RobContentionNehalem : public RobContention {
   private:
      const CoreModel *m_core_model;
      uint64_t m_cache_block_mask;
      ComponentTime m_now;

      // port contention
      bool ports[DynamicMicroOpNehalem::UOP_PORT_SIZE];
      int ports_generic, ports_generic05;

      std::vector<SubsecondTime> alu_used_until;

   public:
      RobContentionNehalem(const Core *core, const CoreModel *core_model);

      void initCycle(SubsecondTime now);
      bool tryIssue(const DynamicMicroOp &uop);
      bool noMore();
      void doIssue(DynamicMicroOp &uop);
};

#endif // __ROB_CONTENTION_NEHALEM_H
