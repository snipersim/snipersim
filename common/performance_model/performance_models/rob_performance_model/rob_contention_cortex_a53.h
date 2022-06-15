/*
 * This file is covered under the Interval Academic License, see LICENCE.academic
 */

#ifndef __ROB_CONTENTION_CORTEX_A53_H
#define __ROB_CONTENTION_CORTEX_A53_H

#include "rob_contention.h"
#include "contention_model.h"
#include "core_model_cortex_a53.h"
#include "dynamic_micro_op_cortex_a53.h"

#include <vector>

class RobContentionCortexA53 : public RobContention {
   private:
      const CoreModel *m_core_model;
      uint64_t m_cache_block_mask;
      ComponentTime m_now;

      // Port contention
      // maximum 1 instruction through these ports
      bool port_branch;
      bool port_simd0;
      bool port_simd1;
      bool port_integer0; // MAC
      bool port_integer1; // DIV
      bool port_ldst;       // Load and store ports

      //Dual issue slots
      bool slot0;
      bool slot1;

      std::vector<SubsecondTime> alu_used_until;

      DynamicMicroOpCortexA53::uop_alu_t issueIntegerPorts();
      DynamicMicroOpCortexA53::uop_alu_t issueNEONPorts();

   public:
      RobContentionCortexA53(const Core *core, const CoreModel *core_model);

      void initCycle(SubsecondTime now);
      bool tryIssue(const DynamicMicroOp &uop);
      bool noMore();
      void doIssue(DynamicMicroOp &uop);
};

#endif // __ROB_CONTENTION_CORTEX_A53_H
