#include "scheduler_dynamic.h"
#include "simulator.h"
#include "thread_stats_manager.h"
#include "hooks_manager.h"

SchedulerDynamic::SchedulerDynamic(ThreadManager *thread_manager)
   : Scheduler(thread_manager)
   , m_threads_runnable(16)
   , m_in_periodic(false)
{
   Sim()->getHooksManager()->registerHook(HookType::HOOK_PERIODIC, hook_periodic, (UInt64)this, HooksManager::ORDER_ACTION);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_START, hook_thread_start, (UInt64)this, HooksManager::ORDER_ACTION);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_STALL, hook_thread_stall, (UInt64)this, HooksManager::ORDER_ACTION);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_RESUME, hook_thread_resume, (UInt64)this, HooksManager::ORDER_ACTION);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_EXIT, hook_thread_exit, (UInt64)this, HooksManager::ORDER_ACTION);
}

SchedulerDynamic::~SchedulerDynamic()
{
}

void SchedulerDynamic::__periodic(SubsecondTime time)
{
   Sim()->getThreadStatsManager()->update();

   m_in_periodic = true;
   periodic(time);
   m_in_periodic = false;
}

void SchedulerDynamic::__threadStart(thread_id_t thread_id, SubsecondTime time)
{
   if (m_threads_runnable.size() <= (size_t)thread_id)
      m_threads_runnable.resize(m_threads_runnable.size() + 16);

   m_threads_runnable[thread_id] = true;
   threadStart(thread_id, time);
}

void SchedulerDynamic::__threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time)
{
   if (reason != ThreadManager::STALL_UNSCHEDULED)
   {
      m_threads_runnable[thread_id] = false;
      threadStall(thread_id, reason, time);
   }
}

void SchedulerDynamic::__threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time)
{
   m_threads_runnable[thread_id] = true;
   threadResume(thread_id, thread_by, time);
}

void SchedulerDynamic::__threadExit(thread_id_t thread_id, SubsecondTime time)
{
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

   m_thread_manager->moveThread(thread_id, core_id, time);
   Sim()->getThreadStatsManager()->update(thread_id, time);
}
