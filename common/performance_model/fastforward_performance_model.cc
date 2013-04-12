#include "fastforward_performance_model.h"
#include "fastforward_performance_manager.h"
#include "simulator.h"
#include "core.h"
#include "sampling_manager.h"
#include "stats.h"

FastforwardPerformanceModel::FastforwardPerformanceModel(Core *core, PerformanceModel *perf)
   : m_core(core)
   , m_perf(perf)
   , m_cpi(SubsecondTime::Zero())
   , m_fastforwarded_time(SubsecondTime::Zero())
{
   registerStatsMetric("fastforward_performance_model", core->getId(), "fastforwarded_time", &m_fastforwarded_time);
   registerStatsMetric("performance_model", core->getId(), "cpiFastforwardTime", &m_fastforwarded_time);
}

void
FastforwardPerformanceModel::incrementElapsedTime(SubsecondTime latency)
{
   m_fastforwarded_time += latency;
   m_perf->incrementElapsedTime(latency);
}

void
FastforwardPerformanceModel::notifyElapsedTimeUpdate()
{
   // After skipping ahead possibly millions of cycles, recalibrate our aim point
   Sim()->getSamplingManager()->recalibrateInstructionsCallback(m_core->getId());
   if (Sim()->getFastForwardPerformanceManager())
      Sim()->getFastForwardPerformanceManager()->recalibrateInstructionsCallback(m_core->getId());
}

void
FastforwardPerformanceModel::countInstructions(IntPtr address, UInt32 count)
{
   incrementElapsedTime(count * m_cpi);
}

void
FastforwardPerformanceModel::queueDynamicInstruction(Instruction *instr)
{
   LOG_ASSERT_ERROR(!instr->isIdle(), "Expected non-idle instruction, got %s", INSTRUCTION_NAMES[instr->getType()]);

   SubsecondTime ffwdTime = instr->getCost(m_core);
   incrementElapsedTime(ffwdTime);

   notifyElapsedTimeUpdate();
}

void
FastforwardPerformanceModel::queueBasicBlock(BasicBlock *basic_block)
{
}
