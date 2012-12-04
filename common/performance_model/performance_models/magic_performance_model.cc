#include "simulator.h"
#include "core.h"
#include "log.h"
#include "magic_performance_model.h"
#include "dvfs_manager.h"

using std::endl;

MagicPerformanceModel::MagicPerformanceModel(Core *core)
    : PerformanceModel(core)
{
}

MagicPerformanceModel::~MagicPerformanceModel()
{
}

bool MagicPerformanceModel::handleInstruction(Instruction const* instruction)
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

   SubsecondTime instruction_cost = instruction->getCost(getCore());
   if (instruction_cost == PerformanceModel::DyninsninfoNotAvailable())
      return false;
   if (isModeled(instruction))
      cost.addLatency(instruction_cost);
   else
      cost.addCycleLatency(1);

   // update counters
   m_instruction_count++;
   m_elapsed_time += cost;

   return true;
}

bool MagicPerformanceModel::isModeled(Instruction const* instruction) const
{
   return instruction->isDynamic();
}
