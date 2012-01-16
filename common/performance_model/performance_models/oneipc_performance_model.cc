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
    , m_instruction_count(0)
    , m_elapsed_time(Sim()->getDvfsManager()->getCoreDomain(core->getId()))
    , m_idle_elapsed_time(Sim()->getDvfsManager()->getCoreDomain(core->getId()))
{
   /* Maximum latency which is assumed to be completely overlapped. Can be set using
      perf_model/core/iocoom/latency_cutoff, else L1-D hit time, else 3 cycles */
   m_latency_cutoff =
      Sim()->getCfg()->getInt("perf_model/core/iocoom/latency_cutoff",
      Sim()->getCfg()->getInt("perf_model/l1_dcache/data_access_time",
      3));
   registerStatsMetric("performance_model", core->getId(), "instruction_count", &m_instruction_count);
   registerStatsMetric("performance_model", core->getId(), "elapsed_time", &m_elapsed_time);
   registerStatsMetric("performance_model", core->getId(), "idle_elapsed_time", &m_idle_elapsed_time);
}

OneIPCPerformanceModel::~OneIPCPerformanceModel()
{
}

void OneIPCPerformanceModel::outputSummary(std::ostream &os) const
{
   os << "  Instructions: " << getInstructionCount() << std::endl
      << "  Cycles: " << m_elapsed_time.getCycleCount() << std::endl
      << "  Time: " << m_elapsed_time.getElapsedTime().getNS() << std::endl;

   if (getConstBranchPredictor())
      getConstBranchPredictor()->outputSummary(os);
}

bool OneIPCPerformanceModel::handleInstruction(Instruction const* instruction)
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

   if (instruction->getType() == INST_SYNC || instruction->getType() == INST_RECV)
      m_idle_elapsed_time.addLatency(cost);

   // update counters
   m_instruction_count++;
   m_elapsed_time.addLatency(cost);

   return true;
}

bool OneIPCPerformanceModel::isModeled(Instruction const* instruction) const
{
   // Arithmetic instructions, branches, are all "not modeled" == unit cycle latency
   // Dynamic instructions (SYNC, MEMACCESS, etc.): normal latency
   // TODO: Shouldn't we handle String instructions as well?
   return instruction->isDynamic();
}

void OneIPCPerformanceModel::setElapsedTime(SubsecondTime time)
{
   LOG_ASSERT_ERROR((time >= m_elapsed_time.getElapsedTime()) || (m_elapsed_time.getElapsedTime() == SubsecondTime::Zero()),
         "time(%s) < m_elapsed_time(%s)",
         itostr(time).c_str(),
         itostr(m_elapsed_time.getElapsedTime()).c_str());
   m_idle_elapsed_time.setElapsedTime(time - m_elapsed_time.getElapsedTime());
   m_elapsed_time.setElapsedTime(time);
}

void OneIPCPerformanceModel::incrementElapsedTime(SubsecondTime time)
{
   m_elapsed_time.addLatency(time);
}
