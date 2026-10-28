/*
 * This file is covered under the Interval Academic License, see LICENCE.academic
 */

#ifndef __ROB_CONTENTION_CORTEX_A72_H
#define __ROB_CONTENTION_CORTEX_A72_H

#include "rob_contention.h"
#include "contention_model.h"
#include "core_model_cortex_a72.h"
#include "dynamic_micro_op_cortex_a72.h"

#include <vector>

class RobContentionCortexA72 : public RobContention {
   private:
      const CoreModel *m_core_model;
      uint64_t m_cache_block_mask;
      ComponentTime m_now;

      // Port contention: how many µOPs of each kind issue per cycle, and the
      // limits (perf_model/core/cortex_a72_ports, the A72's by default)
      unsigned int m_branch, m_integer, m_multi, m_fp, m_load, m_store, m_memory;
      unsigned int n_branch, n_integer, n_multi, n_fp, n_load, n_store;
      bool m_alu_on_multi;   // single-cycle µOPs also issue to a free multi-cycle pipeline

      std::vector<SubsecondTime> alu_used_until;

   public:
      RobContentionCortexA72(const Core *core, const CoreModel *core_model);

      void initCycle(SubsecondTime now);
      bool tryIssue(const DynamicMicroOp &uop);
      bool noMore();
      void doIssue(DynamicMicroOp &uop);
};

#endif // __ROB_CONTENTION_CORTEX_A72_H
