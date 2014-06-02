#include "rob_smt_performance_model.h"
#include "config.hpp"

std::unordered_map<core_id_t, RobSmtTimer*> RobSmtPerformanceModel::s_rob_timers;

RobSmtTimer* RobSmtPerformanceModel::getRobTimer(Core *core, RobSmtPerformanceModel *perf, const CoreModel *core_model)
{
   uint32_t smt_threads = Sim()->getCfg()->getIntArray("perf_model/core/logical_cpus", core->getId());
   core_id_t core_id_master = core->getId() - (core->getId() % smt_threads);
   Sim()->getStatsManager()->logTopology("smt", core->getId(), core_id_master);

   // Non-application threads do not share a core with anyone else
   if (core->getId() >= (core_id_t) Sim()->getConfig()->getApplicationCores())
      smt_threads = 1;

   if (s_rob_timers.count(core_id_master) == 0)
   {
      s_rob_timers[core_id_master] = new RobSmtTimer(
         smt_threads,
         core,
         perf,
         core_model,
         Sim()->getCfg()->getIntArray("perf_model/branch_predictor/mispredict_penalty", core->getId()),
         Sim()->getCfg()->getIntArray("perf_model/core/interval_timer/dispatch_width", core->getId()),
         Sim()->getCfg()->getIntArray("perf_model/core/interval_timer/window_size", core->getId())
      );
   }
   else
   {
      // Only master core_id is used to synchronize, siblings are synchronized by us
      Sim()->getClockSkewMinimizationServer()->setGroup(core->getId(), core_id_master);
   }
   return s_rob_timers[core_id_master];
}

RobSmtPerformanceModel::RobSmtPerformanceModel(Core *core)
    : MicroOpPerformanceModel(core, !Sim()->getCfg()->getBoolArray("perf_model/core/rob_timer/issue_memops_at_issue", core->getId()))
    , m_rob_timer(getRobTimer(core, this, m_core_model))
    , m_enabled(true)
{
   m_thread_id = m_rob_timer->registerThread(core, this);
}

RobSmtPerformanceModel::~RobSmtPerformanceModel()
{
   // Each master thread destructs its group's RobTimer
   if (s_rob_timers.count(getCore()->getId()))
      delete s_rob_timers[getCore()->getId()];
}

boost::tuple<uint64_t,uint64_t> RobSmtPerformanceModel::simulate(const std::vector<DynamicMicroOp*>& insts)
{
   ScopedLock sl(m_rob_timer->m_lock);
   m_rob_timer->pushInstructions(m_thread_id, insts);

   // Don't simulate here, this is done at a safe point in synchronize()
   uint64_t ins; SubsecondTime latency;
   boost::tie(ins, latency) = m_rob_timer->returnLatency(m_thread_id);

   return boost::tuple<uint64_t,uint64_t>(ins, SubsecondTime::divideRounded(latency, m_elapsed_time.getPeriod()));
}

void RobSmtPerformanceModel::synchronize()
{
   ScopedLock sl(m_rob_timer->m_lock);
   // simulate() calls the barrier, and also does the actual simulation
   m_rob_timer->simulate(m_thread_id);
}

void RobSmtPerformanceModel::notifyElapsedTimeUpdate()
{
   ScopedLock sl(m_rob_timer->m_lock);
   m_rob_timer->synchronize(m_thread_id, m_elapsed_time.getElapsedTime());
}

void RobSmtPerformanceModel::enableDetailedModel()
{
   m_enabled = true;
   ScopedLock sl(m_rob_timer->m_lock);
   m_rob_timer->enable();
}

void RobSmtPerformanceModel::disableDetailedModel()
{
   m_enabled = false;
   ScopedLock sl(m_rob_timer->m_lock);
   m_rob_timer->disable();
}
