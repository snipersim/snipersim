#include "sampling_manager.h"
#include "hooks_manager.h"
#include "magic_server.h"
#include "simulator.h"
#include "core.h"
#include "core_manager.h"
#include "thread_manager.h"
#include "performance_model.h"
#include "fastforward_performance_model.h"
#include "config.hpp"
#include "magic_client.h"
#include "sampling_provider.h"

SamplingManager::SamplingManager(void)
   : m_sampling_enabled(Sim()->getCfg()->getBool("sampling/enabled"))
   , m_fastforward(false)
   , m_warmup(false)
   , m_target_ffend(SubsecondTime::Zero())
   , m_sampling_provider(NULL)
   , m_sampling_algorithm(NULL)
   , m_instructions(Sim()->getConfig()->getApplicationCores(), 0)
   , m_time_total(Sim()->getConfig()->getApplicationCores(), SubsecondTime::Zero())
   , m_time_nonidle(Sim()->getConfig()->getApplicationCores(), SubsecondTime::Zero())
{
   if (! m_sampling_enabled)
      return;

   m_uncoordinated = Sim()->getCfg()->getBool("sampling/uncoordinated");

   LOG_ASSERT_ERROR(Sim()->getConfig()->getSimulationMode() == Config::PINTOOL, "Sampling is only supported in Pin mode");

   Sim()->getHooksManager()->registerHook(HookType::HOOK_INSTR_COUNT, (HooksManager::HookCallbackFunc)SamplingManager::hook_instr_count, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_PERIODIC, (HooksManager::HookCallbackFunc)SamplingManager::hook_periodic, (UInt64)this);

   m_sampling_provider = SamplingProvider::create();
   m_sampling_algorithm = SamplingAlgorithm::create(this);
}

SamplingManager::~SamplingManager(void)
{
   if (m_sampling_provider)
      delete m_sampling_provider;
   if (m_sampling_algorithm)
      delete m_sampling_algorithm;
}

void
SamplingManager::periodic(SubsecondTime time)
{
   if (m_fastforward)
      m_sampling_algorithm->callbackFastForward(time, m_warmup);
   else
      m_sampling_algorithm->callbackDetailed(time);

   #if 0
   if (m_fastforward) {
      // Debug: print out a skew report
      printf("[SKEW] %lu", Timer::now());
      UInt64 t = Sim()->getCoreManager()->getCoreFromID(0)->getPerformanceModel()->getElapsedTime().getNS();
      for(unsigned int c = 0; c < Sim()->getConfig()->getApplicationCores(); ++c)
         printf(" %ld", Sim()->getCoreManager()->getCoreFromID(c)->getPerformanceModel()->getElapsedTime().getNS() - t);
      printf("\n");
   }
   #endif
}

void
SamplingManager::setInstrumentationMode(InstMode::inst_mode_t mode)
{
   // We're counting on the barrier for callbacks (time is still running),
   // so tell Simulator not to disable it even when going out of DETAILED.
   Sim()->setInstrumentationMode(mode, false /* update_barrier */);
}

void
SamplingManager::disableFastForward()
{
   m_fastforward = false;
   m_warmup = false;
   SubsecondTime barrier_next = SubsecondTime::Zero();
   SubsecondTime core_last = SubsecondTime::MaxTime();
   for(unsigned int core_id = 0; core_id < Sim()->getConfig()->getApplicationCores(); ++core_id)
   {
      Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
      core->getPerformanceModel()->setFastForward(false, true);
      core->disableInstructionsCallback();
      if (Sim()->getThreadManager()->isThreadRunning(core_id)) {
         barrier_next = std::max(barrier_next, core->getPerformanceModel()->getElapsedTime());
         core_last = std::min(core_last, core->getPerformanceModel()->getElapsedTime());
      }
   }
   if (m_uncoordinated) {
      for(UInt32 core_id = 0; core_id < Sim()->getConfig()->getApplicationCores(); core_id++)
      {
         // In uncoordinated mode, some cores will be behind because they didn't reach their target instruction count.
         // Reset all their times to the same value so they won't have to catch up in detailed mode.
         PerformanceModel *perf = Sim()->getCoreManager()->getCoreFromID(core_id)->getPerformanceModel();
         perf->getFastforwardPerformanceModel()->incrementElapsedTime(barrier_next - perf->getElapsedTime());
      }
   }
   // Tell the barrier to go to detailed mode again, but make sure it skips over the fastforwarded section of time.
   // FIXME:
   // We restart the barrier at the maximum time, and have all threads make uncoordinated progress
   // towards the fastest thread. There won't be any HOOK_PERIODIC calls (which may not make much sense anyway,
   // as during this period at least one thread is way ahead and won't be running).
   // Alternatively, we could reset the barrier to the min time of all cores, this way, all cores that are behind
   // the fastest one, will start to make detailed progress again in lockstep. We'll also get HOOK_PERIODIC calls
   // during this time.
   if (Sim()->getClockSkewMinimizationServer())
      Sim()->getClockSkewMinimizationServer()->setFastForward(false, barrier_next);
   this->setInstrumentationMode(InstMode::DETAILED);
}

void
SamplingManager::enableFastForward(SubsecondTime until, bool warmup, bool detailed_sync)
{
   m_fastforward = true;
   m_warmup = warmup;
   // Approximate time we want to leave fastforward mode
   m_target_ffend = until;

   SubsecondTime barrier_next = SubsecondTime::Zero();
   for(unsigned int core_id = 0; core_id < Sim()->getConfig()->getApplicationCores(); ++core_id)
   {
      PerformanceModel *perf = Sim()->getCoreManager()->getCoreFromID(core_id)->getPerformanceModel();
      perf->setFastForward(true, detailed_sync);
      barrier_next = std::max(barrier_next, perf->getElapsedTime());
      recalibrateInstructionsCallback(core_id);
   }
   // Set barrier to fastforward, and update next_barrier_time to the maximum of all core times so we definitely release everyone
   if (Sim()->getClockSkewMinimizationServer())
      Sim()->getClockSkewMinimizationServer()->setFastForward(true, barrier_next);

   if (m_warmup)
      this->setInstrumentationMode(InstMode::CACHE_ONLY);
   else
      this->setInstrumentationMode(InstMode::FAST_FORWARD);

   if (m_sampling_provider)
   {
      m_sampling_provider->startSampling(until);
   }
}

SubsecondTime
SamplingManager::getCoreHistoricCPI(Core *core, bool non_idle, SubsecondTime min_nonidle_time) const
{
   UInt64 d_instrs = 0;
   SubsecondTime d_time = SubsecondTime::Zero();

   d_instrs = core->getPerformanceModel()->getInstructionCount() - m_instructions[core->getId()];
   if (non_idle)
      d_time = core->getPerformanceModel()->getNonIdleElapsedTime() - m_time_nonidle[core->getId()];
   else
      d_time = core->getPerformanceModel()->getElapsedTime() - m_time_total[core->getId()];

   if (d_instrs == 0)
   {
      return SubsecondTime::MaxTime();
   }
   else if (d_time <= min_nonidle_time)
   {
      return SubsecondTime::Zero();
   }
   else
   {
      return d_time / d_instrs;
   }
}

void
SamplingManager::resetCoreHistoricCPIs()
{
   for(UInt32 core_id = 0; core_id < Sim()->getConfig()->getApplicationCores(); core_id++)
   {
      Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
      m_instructions[core->getId()] = core->getPerformanceModel()->getInstructionCount();
      m_time_total  [core->getId()] = core->getPerformanceModel()->getElapsedTime();
      m_time_nonidle[core->getId()] = core->getPerformanceModel()->getNonIdleElapsedTime();
   }
}

void
SamplingManager::recalibrateInstructionsCallback(core_id_t core_id)
{
   Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
   SubsecondTime now = core->getPerformanceModel()->getElapsedTime();
   if (now > m_target_ffend) {
      // Just a single instruction so we call into the barrier immediately
      core->setInstructionsCallback(1);
   } else {
      SubsecondTime cpi = core->getPerformanceModel()->getFastforwardPerformanceModel()->getCurrentCPI();
      // If CPI hasn't been set up, fall back to 1 IPC to avoid division by zero
      if (cpi == SubsecondTime::Zero())
         cpi = Sim()->getCoreManager()->getCoreFromID(core_id)->getDvfsDomain()->getPeriod();
      UInt64 ninstrs = SubsecondTime::divideRounded(m_target_ffend - now, cpi);
      core->setInstructionsCallback(ninstrs);
   }
}

void
SamplingManager::instr_count(core_id_t core_id)
{
   if (m_fastforward && Sim()->getMagicServer()->inROI()) {
      if (m_uncoordinated) {
         for(UInt32 core_id = 0; core_id < Sim()->getConfig()->getApplicationCores(); core_id++)
         {
            // In uncoordinated mode, the first processor to reach his target instruction count
            // ends fast-forward mode for everyone
            Sim()->getCoreManager()->getCoreFromID(core_id)->setInstructionsCallback(1);
         }
      }
      Sim()->getCoreManager()->getCoreFromID(core_id)->getClockSkewMinimizationClient()->synchronize(SubsecondTime::Zero(), true);
   }
}
