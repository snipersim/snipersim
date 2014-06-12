#include "fastforward_performance_model.h"
#include "fastforward_performance_manager.h"
#include "simulator.h"
#include "core.h"
#include "sampling_manager.h"
#include "stats.h"
#include "config.hpp"
#include "instruction.h"

FastforwardPerformanceModel::FastforwardPerformanceModel(Core *core, PerformanceModel *perf)
   : m_core(core)
   , m_perf(perf)
   , m_include_memory_latency(Sim()->getCfg()->getBool("perf_model/fast_forward/oneipc/include_memory_latency"))
   , m_include_branch_mispredict(Sim()->getCfg()->getBool("perf_model/fast_forward/oneipc/include_branch_misprediction"))
   , m_branch_misprediction_penalty(core->getDvfsDomain(), Sim()->getCfg()->getIntArray("perf_model/branch_predictor/mispredict_penalty", core->getId()))
   , m_cpi(SubsecondTime::Zero())
   , m_fastforwarded_time(SubsecondTime::Zero())
{
   registerStatsMetric("fastforward_performance_model", core->getId(), "fastforwarded_time", &m_fastforwarded_time);
   registerStatsMetric("performance_model", core->getId(), "cpiFastforwardTime", &m_fastforwarded_time);

   registerStatsMetric("fastforward_timer", core->getId(), "cpiBase", &m_cpiBase);
   registerStatsMetric("fastforward_timer", core->getId(), "cpiBranchPredictor", &m_cpiBranchPredictor);

   m_cpiDataCache.resize(HitWhere::NUM_HITWHERES, SubsecondTime::Zero());
   for (int h = HitWhere::WHERE_FIRST ; h < HitWhere::NUM_HITWHERES ; h++)
   {
      if (HitWhereIsValid((HitWhere::where_t)h))
      {
         String name = "cpiDataCache" + String(HitWhereString((HitWhere::where_t)h));
         registerStatsMetric("fastforward_timer", core->getId(), name, &(m_cpiDataCache[h]));
      }
   }
}

void
FastforwardPerformanceModel::incrementElapsedTime(SubsecondTime latency)
{
   incrementElapsedTime(latency, m_cpiBase);
}

void
FastforwardPerformanceModel::incrementElapsedTime(SubsecondTime latency, SubsecondTime &cpiComponent)
{
   m_fastforwarded_time += latency;
   cpiComponent += latency;
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
   incrementElapsedTime(count * m_cpi, m_cpiBase);
}

void
FastforwardPerformanceModel::handleMemoryLatency(SubsecondTime latency, HitWhere::where_t hit_where)
{
   if (m_include_memory_latency)
      incrementElapsedTime(latency, m_cpiDataCache[hit_where]);
}

void
FastforwardPerformanceModel::handleBranchMispredict()
{
   if (m_include_branch_mispredict)
      incrementElapsedTime(m_branch_misprediction_penalty.getLatency(), m_cpiBranchPredictor);
}

void
FastforwardPerformanceModel::queuePseudoInstruction(PseudoInstruction *instr)
{
   LOG_ASSERT_ERROR(!instr->isIdle(), "Expected non-idle instruction, got %s", INSTRUCTION_NAMES[instr->getType()]);

   SubsecondTime ffwdTime = instr->getCost(m_core);
   incrementElapsedTime(ffwdTime);

   notifyElapsedTimeUpdate();
}
