#include "thread_stats_manager.h"
#include "simulator.h"
#include "core_manager.h"
#include "performance_model.h"
#include "thread.h"
#include "stats.h"

#include <cstring>

ThreadStatsManager::ThreadStatsManager()
{
   Sim()->getHooksManager()->registerHook(HookType::HOOK_PRE_STAT_WRITE, hook_pre_stat_write, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_START, hook_thread_start, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_STALL, hook_thread_stall, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_RESUME, hook_thread_resume, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_EXIT, hook_thread_exit, (UInt64)this);
}

ThreadStatsManager::~ThreadStatsManager()
{
   for(std::unordered_map<thread_id_t, ThreadStats*>::iterator it = m_threads_stats.begin(); it != m_threads_stats.end(); ++it)
      delete it->second;
}

void ThreadStatsManager::update(thread_id_t thread_id, SubsecondTime time)
{
   if (time == SubsecondTime::MaxTime())
      time = Sim()->getClockSkewMinimizationServer()->getGlobalTime();

   if (thread_id == INVALID_THREAD_ID)
   {
      for(std::unordered_map<thread_id_t, ThreadStats*>::iterator it = m_threads_stats.begin(); it != m_threads_stats.end(); ++it)
         it->second->update(time);
   }
   else
   {
      m_threads_stats[thread_id]->update(time);
   }
}

void ThreadStatsManager::pre_stat_write()
{
   update();
}

void ThreadStatsManager::threadStart(thread_id_t thread_id, SubsecondTime time)
{
   m_threads_stats[thread_id] = new ThreadStats(thread_id, time);
   m_threads_stats[thread_id]->update(time); // initialize statistic counters
}

void ThreadStatsManager::threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time)
{
   m_threads_stats[thread_id]->update(time);
}

void ThreadStatsManager::threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time)
{
   m_threads_stats[thread_id]->update(time);
}

void ThreadStatsManager::threadExit(thread_id_t thread_id, SubsecondTime time)
{
   m_threads_stats[thread_id]->update(time);
}

ThreadStatsManager::ThreadStats::ThreadStats(thread_id_t thread_id, SubsecondTime time)
   : m_thread(Sim()->getThreadManager()->getThreadFromID(thread_id))
   , m_core_id(INVALID_CORE_ID)
   , time_by_core(Sim()->getConfig()->getApplicationCores())
   , insn_by_core(Sim()->getConfig()->getApplicationCores())
   , m_time_last(time)
{
   memset(&m_counts, 0, sizeof(ThreadStatsStruct));
   memset(&m_last, 0, sizeof(ThreadStatsStruct));

   registerStatsMetric("thread", thread_id, "elapsed_time", &m_elapsed_time);
   registerStatsMetric("thread", thread_id, "unscheduled_time", &m_unscheduled_time);
   registerStatsMetric("thread", thread_id, "instruction_count", &m_counts.instructions);
   registerStatsMetric("thread", thread_id, "core_elapsed_time", &m_counts.elapsed_time);
   registerStatsMetric("thread", thread_id, "nonidle_elapsed_time", &m_counts.nonidle_elapsed_time);
   for (core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); core_id++)
   {
      registerStatsMetric("thread", thread_id, "time_by_core[" + itostr(core_id) + "]", &time_by_core[core_id]);
      registerStatsMetric("thread", thread_id, "instructions_by_core[" + itostr(core_id) + "]", &insn_by_core[core_id]);
   }
}

void ThreadStatsManager::ThreadStats::update(SubsecondTime time)
{
   // Increment per-thread statistics based on the progress our core has made since last time
   SubsecondTime time_delta = time - m_time_last;
   if (m_core_id == INVALID_CORE_ID)
   {
      m_elapsed_time += time_delta;
      m_unscheduled_time += time_delta;
   }
   else
   {
      Core *core = Sim()->getCoreManager()->getCoreFromID(m_core_id);
      m_elapsed_time += time_delta;
      m_counts.instructions += core->getPerformanceModel()->getInstructionCount() - m_last.instructions;
      m_counts.elapsed_time += core->getPerformanceModel()->getElapsedTime() - m_last.elapsed_time;
      m_counts.nonidle_elapsed_time += core->getPerformanceModel()->getNonIdleElapsedTime() - m_last.nonidle_elapsed_time;
      time_by_core[core->getId()] += core->getPerformanceModel()->getElapsedTime() - m_last.elapsed_time;
      insn_by_core[core->getId()] += core->getPerformanceModel()->getInstructionCount() - m_last.instructions;
   }
   // Take a snapshot of our current core's statistics for later comparison
   Core *core = m_thread->getCore();
   if (core)
   {
      m_core_id = core->getId();
      m_last.instructions = core->getPerformanceModel()->getInstructionCount();
      m_last.elapsed_time = core->getPerformanceModel()->getElapsedTime();
      m_last.nonidle_elapsed_time = core->getPerformanceModel()->getNonIdleElapsedTime();
   }
   else
      m_core_id = INVALID_CORE_ID;

   m_time_last = time;
}
