#include "fastforward_performance_manager.h"
#include "simulator.h"
#include "config.hpp"
#include "thread.h"
#include "hooks_manager.h"
#include "core_manager.h"
#include "performance_model.h"
#include "fastforward_performance_model.h"

FastForwardPerformanceManager *
FastForwardPerformanceManager::create(void)
{
   String model = Sim()->getCfg()->getString("perf_model/fast_forward/model");

   if (model == "none")
      return NULL;
   else if (model == "oneipc")
      return new FastForwardPerformanceManager();
   else
      LOG_PRINT_ERROR("Unknown fast-forward performance model %s", model.c_str());
}

FastForwardPerformanceManager::FastForwardPerformanceManager()
   : m_sync_interval(SubsecondTime::NS(Sim()->getCfg()->getInt("perf_model/fast_forward/oneipc/interval")))
   , m_enabled(false)
   , m_target_sync_time(SubsecondTime::Zero())
{
   Sim()->getHooksManager()->registerHook(HookType::HOOK_INSTR_COUNT, FastForwardPerformanceManager::hook_instr_count, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_PERIODIC, FastForwardPerformanceManager::hook_periodic, (UInt64)this);
}

void
FastForwardPerformanceManager::enable()
{
   m_enabled = true;
   for (UInt32 i = 0; i < Sim()->getConfig()->getTotalCores(); i++)
      Sim()->getCoreManager()->getCoreFromID(i)->enablePerformanceModels();

   Sim()->getClockSkewMinimizationServer()->setFastForward(true);
   // Make sure all threads are released, as the barrier has just changed from per-core to per-HW-context mode
   Sim()->getClockSkewMinimizationServer()->release();

   step();
}

void
FastForwardPerformanceManager::disable()
{
   m_enabled = false;

   SubsecondTime barrier_next = SubsecondTime::Zero();
   SubsecondTime core_last = SubsecondTime::MaxTime();
   for(unsigned int core_id = 0; core_id < Sim()->getConfig()->getApplicationCores(); ++core_id)
   {
      Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
      core->disablePerformanceModels(); // Probably someone else will turn them on again soon, but let's do this anyway
      core->getPerformanceModel()->setFastForward(false);
      core->disableInstructionsCallback();
      if (core->getThread() && Sim()->getThreadManager()->isThreadRunning(core->getThread()->getId()))
      {
         barrier_next = std::max(barrier_next, core->getPerformanceModel()->getElapsedTime());
         core_last = std::min(core_last, core->getPerformanceModel()->getElapsedTime());
      }
   }
   Sim()->getClockSkewMinimizationServer()->setFastForward(false, barrier_next);
   // If some threads were behind but still caught the barrier (everyone does in fast-forward mode),
   // make sure to release them so they have a chance to make progress and hit the barrier again at the proper time.
   Sim()->getClockSkewMinimizationServer()->release();
}

void
FastForwardPerformanceManager::periodic(SubsecondTime time)
{
   if (m_enabled)
   {
      step();
   }
}

void
FastForwardPerformanceManager::instr_count(core_id_t core_id)
{
   if (m_enabled)
   {
       Sim()->getCoreManager()->getCoreFromID(core_id)->getClockSkewMinimizationClient()->synchronize(SubsecondTime::Zero(), true);

      if (!Sim()->isRunning())
      {
         // Main thread has exited, but we still seem to be running.
         // Don't touch any more simulator structure as they're being deallocated right now.
         // Just wait here until the whole application terminates us.
         while(1)
            sched_yield();
      }
   }
}

void
FastForwardPerformanceManager::step()
{
    // Approximate time we want to leave fastforward mode
   m_target_sync_time = Sim()->getClockSkewMinimizationServer()->getGlobalTime() + m_sync_interval;

   SubsecondTime barrier_next = SubsecondTime::Zero();
   for(unsigned int core_id = 0; core_id < Sim()->getConfig()->getApplicationCores(); ++core_id)
   {
       PerformanceModel *perf = Sim()->getCoreManager()->getCoreFromID(core_id)->getPerformanceModel();
       perf->setFastForward(true);
       barrier_next = std::max(barrier_next, perf->getElapsedTime());
       recalibrateInstructionsCallback(core_id);
    }
    // Set barrier to fastforward, and update next_barrier_time to the maximum of all core times so we definitely release everyone
    Sim()->getClockSkewMinimizationServer()->setFastForward(true, barrier_next);
}

void
FastForwardPerformanceManager::recalibrateInstructionsCallback(core_id_t core_id)
{
   Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
   SubsecondTime now = core->getPerformanceModel()->getElapsedTime();

   if (now > m_target_sync_time)
   {
      // Just a single instruction so we call into the barrier immediately
      core->setInstructionsCallback(1);
   }
   else
   {
      // One IPC
      SubsecondTime cpi = Sim()->getCoreManager()->getCoreFromID(core_id)->getDvfsDomain()->getPeriod();
      UInt64 ninstrs = SubsecondTime::divideRounded(m_target_sync_time - now, cpi);

      core->getPerformanceModel()->getFastforwardPerformanceModel()->setCurrentCPI(cpi);
      core->setInstructionsCallback(ninstrs);
   }
}
