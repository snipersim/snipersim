#include "scheduler_pinned.h"
#include "simulator.h"
#include "core_manager.h"
#include "performance_model.h"
#include "config.hpp"
#include "os_compat.h"

#include <sstream>

// Pinned scheduler.
// Each thread has is pinned to a specific core (m_thread_affinity).
// Cores are handed out to new threads in round-robin fashion.
// If multiple threads share a core, they are time-shared with a configurable quantum

SchedulerPinned::SchedulerPinned(ThreadManager *thread_manager)
   : SchedulerDynamic(thread_manager)
   , m_quantum(SubsecondTime::NS(Sim()->getCfg()->getInt("scheduler/pinned/quantum")))
   , m_interleaving(Sim()->getCfg()->getInt("scheduler/pinned/interleaving"))
   , m_next_core(0)
   , m_last_periodic(SubsecondTime::Zero())
   , m_core_thread_running(Sim()->getConfig()->getApplicationCores(), INVALID_THREAD_ID)
   , m_quantum_left(Sim()->getConfig()->getApplicationCores(), SubsecondTime::Zero())
{
   m_core_mask.resize(Sim()->getConfig()->getApplicationCores());

   for (core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); core_id++)
   {
       m_core_mask[core_id] = Sim()->getCfg()->getBoolArray("scheduler/pinned/core_mask", core_id);
   }
}

core_id_t SchedulerPinned::getNextCore(core_id_t core_id)
{
   core_id += m_interleaving;
   if (core_id > (core_id_t)Sim()->getConfig()->getApplicationCores())
   {
      core_id %= Sim()->getConfig()->getApplicationCores();
      core_id += 1;
      core_id %= m_interleaving;
   }
   return core_id;
}

core_id_t SchedulerPinned::findFreeCoreForThread(thread_id_t thread_id)
{
   for(core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
   {
      if (m_thread_info[thread_id].hasAffinity(core_id) && m_core_thread_running[core_id] == INVALID_THREAD_ID)
      {
         return core_id;
      }
   }
   return INVALID_CORE_ID;
}

core_id_t SchedulerPinned::threadCreate(thread_id_t thread_id)
{
   if (m_thread_info.size() <= (size_t)thread_id)
      m_thread_info.resize(m_thread_info.size() + 16);

   while(!m_core_mask[m_next_core])
      m_next_core = getNextCore(m_next_core);

   core_id_t core_id = m_next_core;
   m_thread_info[thread_id].setAffinitySingle(core_id);

   m_next_core = getNextCore(m_next_core);

   // The first thread scheduled on this core can start immediately, the others have to wait
   if (m_core_thread_running[core_id] == INVALID_THREAD_ID)
   {
      m_thread_info[thread_id].setCoreRunning(core_id);
      m_core_thread_running[core_id] = thread_id;
      m_quantum_left[core_id] = m_quantum;
      return core_id;
   }
   else
   {
      m_thread_info[thread_id].setCoreRunning(INVALID_CORE_ID);
      return INVALID_CORE_ID;
   }
}

void SchedulerPinned::threadYield(thread_id_t thread_id)
{
   core_id_t core_id = m_thread_info[thread_id].getCoreRunning();

   if (core_id != INVALID_CORE_ID)
   {
      Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
      SubsecondTime time = core->getPerformanceModel()->getElapsedTime();

      m_quantum_left[core_id] = SubsecondTime::Zero();
      reschedule(time, core_id, false);

      if (!m_thread_info[thread_id].hasAffinity(core_id))
      {
         core_id_t free_core_id = findFreeCoreForThread(thread_id);
         if (free_core_id != INVALID_CORE_ID)
         {
            // We have just been moved to a different core(s), and one of them is free. Schedule us there now.
            reschedule(time, free_core_id, false);
         }
      }
   }
}

bool SchedulerPinned::threadSetAffinity(thread_id_t calling_thread_id, thread_id_t thread_id, size_t cpusetsize, const cpu_set_t *mask)
{
   if (!mask)
   {
      // No mask given: free to schedule anywhere.
      for(core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
      {
         m_thread_info[thread_id].addAffinity(core_id);
      }
   }
   else
   {
      m_thread_info[thread_id].clearAffinity();
      bool any = false;

      for(unsigned int cpu = 0; cpu < 8 * cpusetsize; ++cpu)
      {
         if (CPU_ISSET_S(cpu, cpusetsize, mask))
         {
            LOG_ASSERT_ERROR(cpu < Sim()->getConfig()->getApplicationCores(), "Invalid core %d found in sched_setaffinity() mask", cpu);
            any = true;

            m_thread_info[thread_id].addAffinity(cpu);
         }
      }

      LOG_ASSERT_ERROR(any, "No valid core found in sched_setaffinity() mask");
   }

   if (thread_id == calling_thread_id)
   {
      threadYield(thread_id);
   }
   else if (m_thread_info[thread_id].isRunning()                           // Thread is running
            && !m_thread_info[thread_id].hasAffinity(m_thread_info[thread_id].getCoreRunning())) // but not where we want it to
   {
      // Reschedule the thread as soon as possible
      m_quantum_left[m_thread_info[thread_id].getCoreRunning()] = SubsecondTime::Zero();
   }
   else if (m_threads_runnable[thread_id]                                  // Thread is runnable
            && !m_thread_info[thread_id].isRunning())                      // Thread is not running (we can't preempt it outside of the barrier)
   {
      core_id_t free_core_id = findFreeCoreForThread(thread_id);
      if (free_core_id != INVALID_THREAD_ID)                               // Thread's new core is free
      {
         // We have just been moved to a different core, and that core is free. Schedule us there now.
         Core *core = Sim()->getCoreManager()->getCoreFromID(free_core_id);
         SubsecondTime time = core->getPerformanceModel()->getElapsedTime();
         reschedule(time, free_core_id, false);
      }
   }

   return true;
}

bool SchedulerPinned::threadGetAffinity(thread_id_t thread_id, size_t cpusetsize, cpu_set_t *mask)
{
   if (cpusetsize*8 < Sim()->getConfig()->getApplicationCores())
   {
      // Not enough space to return result
      return false;
   }

   CPU_ZERO_S(cpusetsize, mask);
   for(core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
   {
      if (m_thread_info[thread_id].hasAffinity(core_id))
         CPU_SET_S(core_id, cpusetsize, mask);
   }

   return true;
}

void SchedulerPinned::threadStart(thread_id_t thread_id, SubsecondTime time)
{
}

void SchedulerPinned::threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time)
{
   // If the running thread becomes unrunnable, schedule someone else
   if (m_thread_info[thread_id].isRunning())
      reschedule(time, m_thread_info[thread_id].getCoreRunning(), false);
}

void SchedulerPinned::threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time)
{
   // If our core is currently idle, schedule us now
   core_id_t free_core_id = findFreeCoreForThread(thread_id);
   if (free_core_id != INVALID_THREAD_ID)
      reschedule(time, free_core_id, false);
}

void SchedulerPinned::threadExit(thread_id_t thread_id, SubsecondTime time)
{
   // If the running thread becomes unrunnable, schedule someone else
   if (m_thread_info[thread_id].isRunning())
      reschedule(time, m_thread_info[thread_id].getCoreRunning(), false);
}

void SchedulerPinned::periodic(SubsecondTime time)
{
   SubsecondTime delta = time - m_last_periodic;

   for(core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
   {
      if (delta > m_quantum_left[core_id] || m_core_thread_running[core_id] == INVALID_THREAD_ID)
      {
         reschedule(time, core_id, true);
      }
      else
      {
         m_quantum_left[core_id] -= delta;
      }
   }

   m_last_periodic = time;
}

void SchedulerPinned::reschedule(SubsecondTime time, core_id_t core_id, bool is_periodic)
{
   if (m_core_thread_running[core_id] != INVALID_THREAD_ID
       && Sim()->getThreadManager()->getThreadState(m_core_thread_running[core_id]) == Core::INITIALIZING)
   {
      // Thread on this core is starting up, don't reschedule it for now
      return;
   }

   thread_id_t new_thread_id = INVALID_THREAD_ID;
   SubsecondTime min_last_scheduled = SubsecondTime::MaxTime();

   for(thread_id_t thread_id = 0; thread_id < (thread_id_t)m_threads_runnable.size(); ++thread_id)
   {
      if (m_thread_info[thread_id].hasAffinity(core_id) && m_threads_runnable[thread_id] == true)
      {
         // Unless we're in periodic(), don't pre-empt threads currently running on a different core
         if (is_periodic
             || !m_thread_info[thread_id].isRunning()
             || m_thread_info[thread_id].getCoreRunning() == core_id)
         {
            // Find thread that was scheduled the longest time ago
            if (m_thread_info[thread_id].getLastScheduled() < min_last_scheduled)
            {
               new_thread_id = thread_id;
               min_last_scheduled = m_thread_info[thread_id].getLastScheduled();
            }
         }
      }
   }

   if (m_core_thread_running[core_id] != new_thread_id)
   {
      // If a thread was running on this core, and we'll schedule another one, unschedule the current one
      thread_id_t thread_now = m_core_thread_running[core_id];
      if (thread_now != INVALID_THREAD_ID)
      {
         m_thread_info[thread_now].setCoreRunning(INVALID_CORE_ID);
         m_thread_info[thread_now].setLastScheduled(time);
         moveThread(thread_now, INVALID_CORE_ID, time);
      }

      // Set core as running this thread *before* we call moveThread(), otherwise the HOOK_THREAD_RESUME callback for this
      // thread might see an empty core, causing a recursive loop of reschedulings
      m_core_thread_running[core_id] = new_thread_id;

      // If we found a new thread to schedule, move it here
      if (new_thread_id != INVALID_THREAD_ID)
      {
         // If thread was running somewhere else: let that core know
         if (m_thread_info[new_thread_id].isRunning())
            m_core_thread_running[m_thread_info[new_thread_id].getCoreRunning()] = INVALID_THREAD_ID;
         // Move thread to this core
         m_thread_info[new_thread_id].setCoreRunning(core_id);
         moveThread(new_thread_id, core_id, time);
      }
   }

   m_quantum_left[core_id] = m_quantum;
}

String SchedulerPinned::ThreadInfo::getAffinityString() const
{
   std::stringstream ss;

   for(core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
   {
      if (hasAffinity(core_id))
      {
         if (ss.str().size() > 0)
            ss << ",";
         ss << core_id;
      }
   }
   return String(ss.str().c_str());
}

void SchedulerPinned::printState()
{
   printf("thread state:");
   for(thread_id_t thread_id = 0; thread_id < (thread_id_t)Sim()->getThreadManager()->getNumThreads(); ++thread_id)
   {
      char state;
      switch(Sim()->getThreadManager()->getThreadState(thread_id))
      {
         case Core::INITIALIZING:
            state = 'I';
            break;
         case Core::RUNNING:
            state = 'R';
            break;
         case Core::STALLED:
            state = 'S';
            break;
         case Core::SLEEPING:
            state = 's';
            break;
         case Core::WAKING_UP:
            state = 'W';
            break;
         case Core::IDLE:
            state = 'I';
            break;
         case Core::BROKEN:
            state = 'B';
            break;
         case Core::NUM_STATES:
         default:
            state = '?';
            break;
      }
      if (m_thread_info[thread_id].isRunning())
      {
         printf(" %c@%d", state, m_thread_info[thread_id].getCoreRunning());
      }
      else
      {
         printf(" %c%c%s", state, m_threads_runnable[thread_id] ? '+' : '_', m_thread_info[thread_id].getAffinityString().c_str());
      }
   }
   printf("  --  core state:");
   for(core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
   {
      if (m_core_thread_running[core_id] == INVALID_THREAD_ID)
         printf(" __");
      else
         printf(" %2d", m_core_thread_running[core_id]);
   }
   printf("\n");
}
