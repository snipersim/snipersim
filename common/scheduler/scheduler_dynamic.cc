#include "scheduler_dynamic.h"
#include "simulator.h"
#include "core_manager.h"
#include "hooks_manager.h"
#include "performance_model.h"
#include "thread.h"
#include "stats.h"
#include <cstring>

SchedulerDynamic::SchedulerDynamic(ThreadManager *thread_manager)
   : Scheduler(thread_manager)
   , m_threads_runnable(16)
   , m_in_periodic(false)
{
   Sim()->getHooksManager()->registerHook(HookType::HOOK_PERIODIC, hook_periodic, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_PRE_STAT_WRITE, hook_pre_stat_write, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_START, hook_thread_start, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_STALL, hook_thread_stall, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_RESUME, hook_thread_resume, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_EXIT, hook_thread_exit, (UInt64)this);
}

SchedulerDynamic::~SchedulerDynamic()
{
   for(std::unordered_map<thread_id_t, ThreadStats*>::iterator it = m_threads_stats.begin(); it != m_threads_stats.end(); ++it)
      delete it->second;
}

void SchedulerDynamic::__periodic(SubsecondTime time)
{
   updateThreadStats();

   m_in_periodic = true;
   periodic(time);
   m_in_periodic = false;
}

void SchedulerDynamic::__pre_stat_write()
{
   updateThreadStats();
}

void SchedulerDynamic::__threadStart(thread_id_t thread_id, SubsecondTime time)
{
   if (m_threads_runnable.size() <= (size_t)thread_id)
      m_threads_runnable.resize(m_threads_runnable.size() + 16);

   m_threads_runnable[thread_id] = true;
   m_threads_stats[thread_id] = new ThreadStats(thread_id, time);
   m_threads_stats[thread_id]->update(time); // initialize statistic counters
   threadStart(thread_id, time);
}

void SchedulerDynamic::__threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time)
{
   m_threads_stats[thread_id]->update(time);
   if (reason != ThreadManager::STALL_UNSCHEDULED)
   {
      m_threads_runnable[thread_id] = false;
      threadStall(thread_id, reason, time);
   }
}

void SchedulerDynamic::__threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time)
{
   m_threads_stats[thread_id]->update(time);
   m_threads_runnable[thread_id] = true;
   threadResume(thread_id, thread_by, time);
}

void SchedulerDynamic::__threadExit(thread_id_t thread_id, SubsecondTime time)
{
   m_threads_stats[thread_id]->update(time);
   m_threads_runnable[thread_id] = false;
   threadExit(thread_id, time);
}

void SchedulerDynamic::moveThread(thread_id_t thread_id, core_id_t core_id, SubsecondTime time)
{
   #if 0
   // TODO: sched_yield and sched_setaffinity also check for rescheduling. There doesn't seem to be
   //       a uniform way of knowing when this is allowed, so drop the check for now
   // Threads will re-check their core_id on return from barrier, or on wakeup.
   // Outside of this, preemption is not possible.
   LOG_ASSERT_ERROR(m_in_periodic
                    || m_threads_runnable[thread_id] == false
                    || Sim()->getThreadManager()->getThreadFromID(thread_id)->getCore() == NULL,
                    "Cannot pre-emptively move or unschedule thread outside of periodic()");
   #endif
   // onThreadStart will initialize the core that was returned by createThread()
   // Don't move threads that are initializing, or onThreadStart will use the wrong core
   LOG_ASSERT_ERROR(Sim()->getThreadManager()->getThreadState(thread_id) != Core::INITIALIZING,
                    "Cannot move thread %d which is in state INITIALIZING", thread_id);

   m_thread_manager->moveThread(thread_id, core_id, time);
   m_threads_stats[thread_id]->update(time);
}

void SchedulerDynamic::updateThreadStats()
{
   SubsecondTime now = Sim()->getClockSkewMinimizationServer()->getGlobalTime();

   for(std::unordered_map<thread_id_t, ThreadStats*>::iterator it = m_threads_stats.begin(); it != m_threads_stats.end(); ++it)
      it->second->update(now);
}

SchedulerDynamic::ThreadStats::ThreadStats(thread_id_t thread_id, SubsecondTime time)
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

void SchedulerDynamic::ThreadStats::update(SubsecondTime time)
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
