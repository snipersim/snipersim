#include "barrier_sync_client.h"
#include "barrier_sync_server.h"
#include "simulator.h"
#include "core_manager.h"
#include "core.h"
#include "thread.h"
#include "performance_model.h"
#include "hooks_manager.h"
#include "syscall_server.h"
#include "config.h"
#include "log.h"
#include "stats.h"
#include "config.hpp"
#include "circular_log.h"

#include <algorithm>

BarrierSyncServer::BarrierSyncServer()
   : m_local_clock_list(Sim()->getConfig()->getApplicationCores(), SubsecondTime::Zero())
   , m_barrier_acquire_list(Sim()->getConfig()->getApplicationCores(), false)
   , m_core_cond(Sim()->getConfig()->getApplicationCores(), NULL)
   , m_core_group(Sim()->getConfig()->getApplicationCores(), INVALID_CORE_ID)
   , m_core_thread(Sim()->getConfig()->getApplicationCores(), INVALID_THREAD_ID)
   , m_global_time(SubsecondTime::Zero())
   , m_fastforward(false)
   , m_disable(false)
{
   try
   {
      m_barrier_interval = SubsecondTime::NS() * (UInt64) Sim()->getCfg()->getInt("clock_skew_minimization/barrier/quantum");
   }
   catch(...)
   {
      LOG_PRINT_ERROR("Error Reading 'clock_skew_minimization/barrier/quantum' from the config file");
   }

   for(core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
      m_core_cond[core_id] = new ConditionVariable();

   m_next_barrier_time = m_barrier_interval;

   // Order our hooks to occur after possible reschedulings (which are done with ORDER_ACTION)
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_EXIT, BarrierSyncServer::hookThreadExit, (UInt64)this, HooksManager::ORDER_NOTIFY_POST);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_STALL, BarrierSyncServer::hookThreadStall, (UInt64)this, HooksManager::ORDER_NOTIFY_POST);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_MIGRATE, BarrierSyncServer::hookThreadMigrate, (UInt64)this, HooksManager::ORDER_NOTIFY_POST);

   registerStatsMetric("barrier", 0, "global_time", &m_global_time);
}

BarrierSyncServer::~BarrierSyncServer()
{
   for(core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
      delete m_core_cond[core_id];
}

void
BarrierSyncServer::synchronize(core_id_t core_id, SubsecondTime time)
{
   ScopedLock sl(Sim()->getThreadManager()->getLock());
   if (m_disable)
      return;

   Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
   core_id_t master_core_id;
   if (m_fastforward)
      master_core_id = core_id;  // In fast-forward, the SMT performance model in not active so every core (HW context) calls into the barrier
   else
      master_core_id = m_core_group[core_id] == INVALID_CORE_ID ? core_id : m_core_group[core_id];
   Core *master_core = Sim()->getCoreManager()->getCoreFromID(core_id);
   thread_id_t thread_me = core->getThread()->getId();

   CLOG("barrier", "Core %d entry (master core %d, thread %d, ffwd %d)", core_id, master_core_id, thread_me, m_fastforward);
   LOG_PRINT("Received 'SIM_BARRIER_WAIT' from Core(%i), Time(%s)", core_id, itostr(time).c_str());

   LOG_ASSERT_ERROR(core->getState() == Core::RUNNING || core->getState() == Core::INITIALIZING, "Core(%i) is not running or initializing at time(%s)", core_id, itostr(time).c_str());
   LOG_ASSERT_ERROR(m_barrier_acquire_list[master_core_id] == false, "Core(%i) or its sibling is already in the barrier (this is thread %d, we have thread %d)", master_core_id, thread_me, m_core_thread[master_core_id]);

   if (time < m_next_barrier_time && !m_fastforward)
   {
      LOG_PRINT("Sent 'SIM_BARRIER_RELEASE' immediately time(%s), m_next_barrier_time(%s)", itostr(time).c_str(), itostr(m_next_barrier_time).c_str());
      // LOG_PRINT_WARNING("core_id(%i), local_clock(%llu), m_next_barrier_time(%llu), m_barrier_interval(%llu)", core_id, time, m_next_barrier_time, m_barrier_interval);
      CLOG("barrier", "Core %d immediate exit", core_id);
      return;
   }

   // One thread entered the barrier, another one can resume
   doRelease(1);

   master_core->getPerformanceModel()->barrierEnter();

   m_local_clock_list[master_core_id] = time;
   m_barrier_acquire_list[master_core_id] = true;
   m_core_thread[master_core_id] = thread_me;

   bool mustWait = true;
   if (isBarrierReached())
      mustWait = barrierRelease(thread_me);

   if (mustWait)
      m_core_cond[master_core_id]->wait(Sim()->getThreadManager()->getLock());
   else
      master_core->getPerformanceModel()->barrierExit();

   CLOG("barrier", "Core %d exit (master core %d, thread %d)", core_id, master_core_id, thread_me);
}

void
BarrierSyncServer::threadExit(HooksManager::ThreadTime *argument)
{
   // Release thread from the barrier
   releaseThread(argument->thread_id);
   // Check to see if we were waiting for this thread
   signal();
}

void
BarrierSyncServer::threadStall(HooksManager::ThreadStall *argument)
{
   // Release thread from the barrier
   releaseThread(argument->thread_id);
   // Check to see if we were waiting for this thread
   signal();
}

void
BarrierSyncServer::threadMigrate(HooksManager::ThreadMigrate *argument)
{
   // Update the migrating thread's time so we'll be sure to release it
   releaseThread(argument->thread_id);
   // Migration due to thread stall/exit will generate another event later, we'll do a signal() then
   // Migration because of pre-emption is done only inside periodic(), we'll return into barrierRelease()
}

void
BarrierSyncServer::releaseThread(thread_id_t thread_id)
{
   for(core_id_t core_id = 0; core_id < (core_id_t) Sim()->getConfig()->getApplicationCores(); core_id++)
   {
      if (m_barrier_acquire_list[core_id] && m_core_thread[core_id] == thread_id)
      {
         // Make sure thread is released on next barrierRelease()
         m_local_clock_list[core_id] = SubsecondTime::Zero();
      }
   }
   // One thread stopped running, release another one now
   doRelease(1);
}

void
BarrierSyncServer::signal()
{
   if (m_disable)
      return;

   if (isBarrierReached())
      barrierRelease(INVALID_THREAD_ID);
}

bool
BarrierSyncServer::isCoreRunning(core_id_t core_id, bool siblings)
{
   Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
   if (core->getState() == Core::RUNNING)
   {
      LOG_ASSERT_ERROR(core->getThread(), "Core (%d) is running but has no thread", core_id);
      if (Sim()->getThreadManager()->isThreadRunning(core->getThread()->getId()))
         return true;
   }


   if (siblings && !m_fastforward)
   {
      for (core_id_t sibling_core_id = 0; sibling_core_id < (core_id_t) Sim()->getConfig()->getApplicationCores(); sibling_core_id++)
      {
         if (m_core_group[sibling_core_id] == core_id)
         {
            if (isCoreRunning(sibling_core_id, false))
               return true;
         }
      }
   }

   return false;
}

void
BarrierSyncServer::advance()
{
   barrierRelease(INVALID_THREAD_ID, true);
}

bool
BarrierSyncServer::isBarrierReached()
{
   bool single_core_barrier_reached = false;

   // Check if all cores have reached the barrier
   // All least one core must have (sync_time > m_next_barrier_time)
   for (core_id_t core_id = 0; core_id < (core_id_t) Sim()->getConfig()->getApplicationCores(); core_id++)
   {
      // In fastforward mode, it's enough that a core is waiting. In detailed mode, it needs to have advanced up to the predefined barrier time
      if (m_fastforward)
      {
         if (m_barrier_acquire_list[core_id])
         {
            // At least one core has reached the barrier
            single_core_barrier_reached = true;
         }
         else if (isCoreRunning(core_id))
         {
            // Core is running but hasn't checked in yet. Wait for it to sync.
            return false;
         }
      }
      else if (m_core_group[core_id] != INVALID_CORE_ID)
      {
         // Only consider group masters
         continue;
      }
      else if (isCoreRunning(core_id))
      {
         if (m_local_clock_list[core_id] < m_next_barrier_time)
         {
            // Core running on this core has not reached the barrier
            // Wait for it to sync
            return false;
         }
         else
         {
            // At least one core has reached the barrier
            single_core_barrier_reached = true;
         }
      }
   }

   return single_core_barrier_reached;
}

bool
BarrierSyncServer::barrierRelease(thread_id_t caller_id, bool continue_until_release)
{
   CLOG("barrier", "Release (caller thread %d)", caller_id);
   LOG_PRINT("Sending 'BARRIER_RELEASE'");

   // All cores have reached the barrier
   // Advance m_next_barrier_time
   // Release the Barrier

   LOG_ASSERT_ERROR(m_to_release.size() == 0, "Reached the barrier while some threads haven't even restarted?");

   if (m_fastforward)
   {
      for (core_id_t core_id = 0; core_id < (core_id_t) Sim()->getConfig()->getApplicationCores(); core_id++)
      {
         // In fast-forward mode, skip over (potentially very many) timeslots
         if (m_local_clock_list[core_id] > m_next_barrier_time)
            m_next_barrier_time = m_local_clock_list[core_id];
      }
   }

   // If a core cannot be resumed, we have to advance the sync
   // time till a core can be resumed. Then only, will we have
   // forward progress

   bool core_resumed = false;
   bool must_wait = true;
   while (!core_resumed)
   {
      m_global_time = m_next_barrier_time;
      CLOG("barrier", "Barrier %" PRId64 "ns", m_next_barrier_time.getNS());
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

      for (core_id_t core_id = 0; core_id < (core_id_t) Sim()->getConfig()->getApplicationCores(); core_id++)
      {
         if (m_local_clock_list[core_id] < m_next_barrier_time)
         {
            // Check if this core was running. If yes, release that core
            if (m_barrier_acquire_list[core_id] == true)
            {
               //Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
               //LOG_ASSERT_ERROR(core->getState() == Core::RUNNING || core->getState() == Core::INITIALIZING, "(%i) has acquired barrier, local_clock(%s), m_next_barrier_time(%s), but not initializing or running", core_id, itostr(m_local_clock_list[core_id]).c_str(), itostr(m_next_barrier_time).c_str());

               m_barrier_acquire_list[core_id] = false;
               core_resumed = true;

               if (m_core_thread[core_id] == caller_id)
                  must_wait = false;
               else
               {
                  Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
                  core->getPerformanceModel()->barrierExit();
                  m_to_release.push_back(core_id);
               }
            }
         }
      }
   }

   // To avoid overwhelming the OS scheduler, we only release N threads at a time (N ~= host cores).
   // Once a thread is done (stops executing because it completed the next barrier quantum, or due to thread stall),
   // one more thread is released so we always have at most N running threads.
   std::random_shuffle(m_to_release.begin(), m_to_release.end());
   doRelease(m_fastforward ? -1 : Sim()->getConfig()->getNumHostCores());

   return must_wait;
}

void
BarrierSyncServer::doRelease(int n)
{
   // Release up to n threads from the list.
   // When n == -1, all threads are released
   while(m_to_release.size() && n--)
   {
      core_id_t core_id = m_to_release.back();
      m_to_release.pop_back();
      m_core_cond[core_id]->signal();
   }
}

void
BarrierSyncServer::abortBarrier()
{
   CLOG("barrier", "Abort");
   for(core_id_t core_id = 0; core_id < (core_id_t) Sim()->getConfig()->getApplicationCores(); core_id++)
   {
      // Check if this core was running. If yes, release that core
      if (m_barrier_acquire_list[core_id] == true)
      {
         m_barrier_acquire_list[core_id] = false;

         Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
         core->getPerformanceModel()->barrierExit();
         m_core_cond[core_id]->signal();
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
BarrierSyncServer::setGroup(core_id_t core_id, core_id_t master_core_id)
{
   if (master_core_id != INVALID_CORE_ID)
      LOG_ASSERT_ERROR(m_barrier_acquire_list[core_id] == false, "Core(%d) is in the barrier, cannot set participate to false", core_id);

   m_core_group[core_id] = master_core_id;
}

void
BarrierSyncServer::setFastForward(bool fastforward, SubsecondTime next_barrier_time)
{
   if (m_fastforward != fastforward)
      CLOG("barrier", "FastForward %d > %d", m_fastforward, fastforward);
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
   for(core_id_t core_id = 0; core_id < (core_id_t) Sim()->getConfig()->getApplicationCores(); core_id++)
   {
      if (m_core_group[core_id] != INVALID_CORE_ID)
         printf(" .");
      else if (m_barrier_acquire_list[core_id] == true)
      {
         if (m_local_clock_list[core_id] >= m_next_barrier_time)
            printf(" ^");
         else
            printf(" A");
      }
      else if (isCoreRunning(core_id))
         printf(" R");
      else
         printf(" _");
   }
   printf("\n");
}
