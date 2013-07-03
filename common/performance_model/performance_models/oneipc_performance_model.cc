#include "oneipc_performance_model.h"
#include "simulator.h"
#include "core.h"
#include "log.h"
#include "config.hpp"
#include "branch_predictor.h"
#include "stats.h"
#include "dvfs_manager.h"
#include "subsecond_time.h"

using std::endl;

OneIPCPerformanceModel::OneIPCPerformanceModel(Core *core)
    : PerformanceModel(core)
{
   /* Maximum latency which is assumed to be completely overlapped. L1-D hit latency should be a good value. */
   m_latency_cutoff = Sim()->getCfg()->getIntArray("perf_model/core/oneipc/latency_cutoff", core->getId());

   registerStatsMetric("oneipc_timer", core->getId(), "cpiBase", &m_cpiBase);
   registerStatsMetric("oneipc_timer", core->getId(), "cpiBranchPredictor", &m_cpiBranchPredictor);

   m_cpiDataCache.resize(HitWhere::NUM_HITWHERES, SubsecondTime::Zero());
   for (int h = HitWhere::WHERE_FIRST ; h < HitWhere::NUM_HITWHERES ; h++)
   {
      String name = "cpiDataCache" + String(HitWhereString((HitWhere::where_t)h));
      registerStatsMetric("oneipc_timer", core->getId(), name, &(m_cpiDataCache[h]));
   }
}

OneIPCPerformanceModel::~OneIPCPerformanceModel()
{
}

bool OneIPCPerformanceModel::handleInstruction(Instruction const* instruction)
{
   // compute cost
   ComponentTime cost = m_elapsed_time.getLatencyGenerator();
   SubsecondTime *cpiComponent = NULL;

   const OperandList &ops = instruction->getOperands();
   for (unsigned int i = 0; i < ops.size(); i++)
   {
      const Operand &o = ops[i];

      if (o.m_type == Operand::MEMORY)
      {
         DynamicInstructionInfo *info = getDynamicInstructionInfo(*instruction);
         if (!info)
            return false;

         if (o.m_direction == Operand::READ)
         {
            LOG_ASSERT_ERROR(info->type == DynamicInstructionInfo::MEMORY_READ,
                             "Expected memory read info, got: %d.", info->type);

            if (info->memory_info.latency
                  > ComponentLatency(getCore()->getDvfsDomain(), m_latency_cutoff).getLatency())
               cost.addLatency(info->memory_info.latency);
            // ignore address
         }
         else
         {
            LOG_ASSERT_ERROR(info->type == DynamicInstructionInfo::MEMORY_WRITE,
                             "Expected memory write info, got: %d.", info->type);

            // ignore write latency
            // ignore address
         }

         if (cpiComponent == NULL)
            cpiComponent = &m_cpiDataCache[info->memory_info.hit_where];

         popDynamicInstructionInfo();
      }
   }

   SubsecondTime instruction_cost = instruction->getCost(getCore());
   if (instruction_cost == PerformanceModel::DyninsninfoNotAvailable())
      return false;

   if (isModeled(instruction))
      cost.addLatency(instruction_cost);
   else
      cost.addLatency(ComponentLatency(getCore()->getDvfsDomain(), 1).getLatency());

   LOG_ASSERT_ERROR((instruction->getType() != INST_SYNC && instruction->getType() != INST_RECV), "Unexpected non-idle instruction");

   if (cpiComponent == NULL)
   {
      if (instruction->getType() == INST_BRANCH)
         cpiComponent = &m_cpiBranchPredictor;
      else
         cpiComponent = &m_cpiBase;
   }

   // update counters
   m_instruction_count++;
   m_elapsed_time.addLatency(cost);

   LOG_ASSERT_ERROR(cpiComponent != NULL, "Expected cpiComponent to be set");
   *cpiComponent += cost.getElapsedTime();

   return true;
}

bool OneIPCPerformanceModel::isModeled(Instruction const* instruction) const
{
   // Arithmetic instructions etc., are all "not modeled" == unit cycle latency
   // Dynamic instructions (SYNC, MEMACCESS, etc.): normal latency
   // Mispredicted branches: penalty as defined by BranchPredictor::getMispredictPenalty()
   // TODO: Shouldn't we handle String instructions as well?
   return instruction->isDynamic() || instruction->getType() == INST_BRANCH;
}
