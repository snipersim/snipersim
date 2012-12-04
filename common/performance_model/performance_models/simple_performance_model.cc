#include "simulator.h"
#include "core.h"
#include "log.h"
#include "simple_performance_model.h"
#include "branch_predictor.h"
#include "dvfs_manager.h"

using std::endl;

SimplePerformanceModel::SimplePerformanceModel(Core *core)
    : PerformanceModel(core)
{
}

SimplePerformanceModel::~SimplePerformanceModel()
{
}

bool SimplePerformanceModel::handleInstruction(Instruction const* instruction)
{
   // compute cost
   ComponentTime cost = m_elapsed_time.getLatencyGenerator();

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

            cost.addLatency(info->memory_info.latency);
            // ignore address
         }
         else
         {
            LOG_ASSERT_ERROR(info->type == DynamicInstructionInfo::MEMORY_WRITE,
                             "Expected memory write info, got: %d.", info->type);

            cost.addLatency(info->memory_info.latency);
            // ignore address
         }

         popDynamicInstructionInfo();
      }
   }

   SubsecondTime i_cost = instruction->getCost(getCore());
   if (i_cost == PerformanceModel::DyninsninfoNotAvailable())
      return false;
   cost.addLatency(instruction->getCost(getCore()));
   // LOG_ASSERT_WARNING(cost < 10000, "Cost is too big - cost:%llu, cycle_count: %llu, type: %d", cost, m_elapsed_time, instruction->getType());

   // update counters
   m_instruction_count++;
   m_elapsed_time += cost;

   return true;
}
