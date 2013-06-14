#include "barrier_sync_client.h"
#include "barrier_sync_server.h"
#include "simulator.h"
#include "thread_manager.h"
#include "thread.h"
#include "performance_model.h"
#include "hooks_manager.h"
#include "syscall_server.h"
#include "config.h"
#include "log.h"
#include "stats.h"
#include "config.hpp"

BarrierSyncServer::BarrierSyncServer()
   : m_global_time(SubsecondTime::Zero())
   , m_fastforward(false)
   , m_disable(false)
{
   m_thread_manager = Sim()->getThreadManager();
   try
   {
      m_barrier_interval = SubsecondTime::NS() * (UInt64) Sim()->getCfg()->getInt("clock_skew_minimization/barrier/quantum");
   }
   catch(...)
   {
      LOG_PRINT_ERROR("Error Reading 'clock_skew_minimization/barrier/quantum' from the config file");
   }

   m_next_barrier_time = m_barrier_interval;

   registerStatsMetric("barrier", 0, "global_time", &m_global_time);
}

BarrierSyncServer::~BarrierSyncServer()
{}

void
BarrierSyncServer::synchronize(thread_id_t thread_id, SubsecondTime time)
{
   ScopedLock sl(Sim()->getThreadManager()->getLock());
   if (m_disable)
       return;

   LOG_PRINT("Received 'SIM_BARRIER_WAIT' from Thread(%i), Time(%s)", thread_id, itostr(time).c_str());

   LOG_ASSERT_ERROR(m_thread_manager->isThreadRunning(thread_id) || m_thread_manager->isThreadInitializing(thread_id), "Thread(%i) is not running or initializing at time(%s)", thread_id, itostr(time).c_str());

   if (time < m_next_barrier_time && !m_fastforward)
   {
      LOG_PRINT("Sent 'SIM_BARRIER_RELEASE' immediately time(%s), m_next_barrier_time(%s)", itostr(time).c_str(), itostr(m_next_barrier_time).c_str());
      // LOG_PRINT_WARNING("core_id(%i), local_clock(%llu), m_next_barrier_time(%llu), m_barrier_interval(%llu)", core_id, time, m_next_barrier_time, m_barrier_interval);
      return;
   }

   Core *core = Sim()->getThreadManager()->getThreadFromID(thread_id)->getCore();
   core->getPerformanceModel()->barrierEnter();

   m_local_clock_list[thread_id] = time;
   m_barrier_acquire_list[thread_id] = true;

   bool mustWait = true;
   if (isBarrierReached())
      mustWait = barrierRelease(thread_id);

   if (mustWait)
      Sim()->getThreadManager()->getThreadFromID(thread_id)->wait(Sim()->getThreadManager()->getLock());
   else
      core->getPerformanceModel()->barrierExit();
}

void
BarrierSyncServer::signal()
{
   if (m_disable)
      return;
   if (isBarrierReached())
      barrierRelease();
}

void
BarrierSyncServer::advance()
{
   barrierRelease(INVALID_THREAD_ID, true);
}

bool
BarrierSyncServer::isBarrierReached()
{
   bool single_thread_barrier_reached = false;

   // Check if all threads have reached the barrier
   // All least one thread must have (sync_time > m_next_barrier_time)
   for (thread_id_t thread_id = 0; thread_id < (thread_id_t) Sim()->getThreadManager()->getNumThreads(); thread_id++)
   {
      // In fastforward mode, it's enough that a thread is waiting. In detailed mode, it needs to have advanced up to the predefined barrier time
      if (m_fastforward)
      {
         if (m_barrier_acquire_list[thread_id])
         {
            // At least one thread has reached the barrier
            single_thread_barrier_reached = true;
         }
         else if (m_thread_manager->isThreadRunning(thread_id))
         {
            // Thread is running but hasn't checked in yet. Wait for it to sync.
            return false;
         }
      }
      else if (m_thread_manager->isThreadRunning(thread_id))
      {
         if (m_local_clock_list[thread_id] < m_next_barrier_time)
         {
            // Thread Running on this core has not reached the barrier
            // Wait for it to sync
            return false;
         }
         else
         {
            // At least one thread has reached the barrier
            single_thread_barrier_reached = true;
         }
      }
   }

   return single_thread_barrier_reached;
}

bool
BarrierSyncServer::barrierRelease(thread_id_t caller_id, bool continue_until_release)
{
   LOG_PRINT("Sending 'BARRIER_RELEASE'");

   // All threads have reached the barrier
   // Advance m_next_barrier_time
   // Release the Barrier

   if (m_fastforward)
   {
      for (thread_id_t thread_id = 0; thread_id < (thread_id_t) Sim()->getThreadManager()->getNumThreads(); thread_id++)
      {
         // In fast-forward mode, skip over (potentially very many) timeslots
         if (m_local_clock_list[thread_id] > m_next_barrier_time)
            m_next_barrier_time = m_local_clock_list[thread_id];
      }
   }

   // If a thread cannot be resumed, we have to advance the sync
   // time till a thread can be resumed. Then only, will we have
   // forward progress

   bool thread_resumed = false;
   bool must_wait = true;
   while (!thread_resumed)
   {
      m_global_time = m_next_barrier_time;
      Sim()->getHooksManager()->callHooks(HookType::HOOK_PERIODIC, static_cast<subsecond_time_t>(m_next_barrier_time).m_time);

      if (continue_until_release)
      {
         // If HOOK_PERIODIC woke someone up, this thread can safely go to sleep
         if (Sim()->getThreadManager()->anyThreadRunning())
            return false;
         else
            LOG_ASSERT_ERROR(Sim()->getSyscallServer()->getNextTimeout(m_global_time) < SubsecondTime::MaxTime(), "No threads running, no timeout. Application has deadlocked...");
      }

      // If the barrier was disabled from HOOK_PERIODIC (for instance, if roi-end was triggered from a script), break
      if (m_disable)
         return false;

      m_next_barrier_time += m_barrier_interval;
      LOG_PRINT("m_next_barrier_time updated to (%s)", itostr(m_next_barrier_time).c_str());

      for (thread_id_t thread_id = 0; thread_id < (thread_id_t) Sim()->getThreadManager()->getNumThreads(); thread_id++)
      {
         if (m_local_clock_list[thread_id] < m_next_barrier_time)
         {
            // Check if this core was running. If yes, release that core
            if (m_barrier_acquire_list[thread_id] == true)
            {
               //LOG_ASSERT_ERROR(m_thread_manager->isThreadRunning(thread_id) || m_thread_manager->isThreadInitializing(thread_id), "(%i) has acquired barrier, local_clock(%s), m_next_barrier_time(%s), but not initializing or running", thread_id, itostr(m_local_clock_list[thread_id]).c_str(), itostr(m_next_barrier_time).c_str());

               m_barrier_acquire_list[thread_id] = false;
               thread_resumed = true;

               if (thread_id == caller_id)
                  must_wait = false;
               else
               {
                  Thread *thread = Sim()->getThreadManager()->getThreadFromID(thread_id);
                  if (thread->getCore())
                     thread->getCore()->getPerformanceModel()->barrierExit();
                  thread->signal(m_local_clock_list[thread_id]);
               }
            }
         }
      }
   }
   return must_wait;
}

void
BarrierSyncServer::abortBarrier()
{
   for(thread_id_t thread_id = 0; thread_id < (thread_id_t) Sim()->getThreadManager()->getNumThreads(); thread_id++)
   {
      // Check if this core was running. If yes, release that core
      if (m_barrier_acquire_list[thread_id] == true)
      {
         m_barrier_acquire_list[thread_id] = false;

         Thread *thread = Sim()->getThreadManager()->getThreadFromID(thread_id);
         if (thread->getCore())
            thread->getCore()->getPerformanceModel()->barrierExit();
         thread->signal(m_local_clock_list[thread_id]);
      }
   }
}

void
BarrierSyncServer::setDisable(bool disable)
{
   this->m_disable = disable;
   if (disable)
      abortBarrier();
}

void
BarrierSyncServer::setFastForward(bool fastforward, SubsecondTime next_barrier_time)
{
   m_fastforward = fastforward;
   if (next_barrier_time != SubsecondTime::MaxTime())
   {
      m_next_barrier_time = std::max(m_next_barrier_time, next_barrier_time);
   }
}

void
BarrierSyncServer::printState(void)
{
   printf("Barrier state:");
   for(thread_id_t thread_id = 0; thread_id < (thread_id_t) Sim()->getThreadManager()->getNumThreads(); thread_id++)
   {
      if (m_local_clock_list[thread_id] >= m_next_barrier_time)
         printf(" ^");
      else if (m_barrier_acquire_list[thread_id] == true)
         printf(" A");
      else if (m_thread_manager->isThreadRunning(thread_id))
         printf(" R");
      else
         printf(" _");
   }
   printf("\n");
}
