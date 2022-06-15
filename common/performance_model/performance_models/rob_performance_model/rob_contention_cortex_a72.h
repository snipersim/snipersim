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

      // Port contention
      // maximum 1 instruction through these ports
      bool port_branch;
      bool port_simd0;
      bool port_simd1;
      bool port_int_multi;  // Integer multi-cycle
      bool port_ld;       // Load port
      bool port_st;       // Store port
      // maximum 2 instruction through these ports
      int ports_integer;    // Integer 0/1

      std::vector<SubsecondTime> alu_used_until;

   public:
      RobContentionCortexA72(const Core *core, const CoreModel *core_model);

      void initCycle(SubsecondTime now);
      bool tryIssue(const DynamicMicroOp &uop);
      bool noMore();
      void doIssue(DynamicMicroOp &uop);
};

#endif // __ROB_CONTENTION_CORTEX_A72_H
