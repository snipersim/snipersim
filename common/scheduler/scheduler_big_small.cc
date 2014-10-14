#include "scheduler_big_small.h"
#include "simulator.h"
#include "config.hpp"
#include "thread.h"
#include "performance_model.h"
#include "core_manager.h"
#include "misc/tags.h"
#include "rng.h"

// Example random big-small scheduler using thread affinity
//
// The hardware consists of B big cores and S small cores
// This scheduler selects B threads that have affinity to (all) big cores,
// while the other threads have affinity to the small cores.
// Periodically, or when threads on a big core stall/end,
// new threads are promoted to run on the big core(s)
//
// This scheduler uses only setThreadAffinity(), and leaves all messy
// low-level details that govern thread stalls etc. to the implementation
// of SchedulerPinnedBase

SchedulerBigSmall::SchedulerBigSmall(ThreadManager *thread_manager)
   : SchedulerPinnedBase(thread_manager, SubsecondTime::NS(Sim()->getCfg()->getInt("scheduler/big_small/quantum")))
   , m_debug_output(Sim()->getCfg()->getBool("scheduler/big_small/debug"))
   , m_last_reshuffle(SubsecondTime::Zero())
   , m_rng(rng_seed(42))
{
   // Figure out big and small cores, and create affinity masks for the set of big cores and the set of small cores, respectively

   m_num_big_cores = 0;
   CPU_ZERO(&m_mask_big);
   CPU_ZERO(&m_mask_small);

   for (core_id_t coreId = 0; coreId < (core_id_t) Sim()->getConfig()->getApplicationCores(); coreId++)
   {
      bool isBig = Sim()->getTagsManager()->hasTag("core", coreId, "big");
      if (isBig)
      {
         ++m_num_big_cores;
         CPU_SET(coreId, &m_mask_big);
      }
      else
      {
         CPU_SET(coreId, &m_mask_small);
      }
   }
}

void SchedulerBigSmall::threadSetInitialAffinity(thread_id_t thread_id)
{
   // All threads start out on the small core(s)
   moveToSmall(thread_id);
}

void SchedulerBigSmall::threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time)
{
   // When a thread on the big core stalls, promote another thread to the big core(s)

   if (m_debug_output)
      std::cout << "[SchedulerBigSmall] thread " << thread_id << " stalled" << std::endl;

   if (m_thread_isbig[thread_id])
   {
      // Pick a new thread to run on the big core(s)
      pickBigThread();
      // Move this thread to the small core(s)
      moveToSmall(thread_id);
   }

   // Call threadStall() in parent class
   SchedulerPinnedBase::threadStall(thread_id, reason, time);

   if (m_debug_output)
      printState();
}

void SchedulerBigSmall::threadExit(thread_id_t thread_id, SubsecondTime time)
{
   // When a thread on the big core ends, promote another thread to the big core(s)

   if (m_debug_output)
      std::cout << "[SchedulerBigSmall] thread " << thread_id << " ended" << std::endl;

   if (m_thread_isbig[thread_id])
   {
      // Pick a new thread to run on the big core(s)
      pickBigThread();
   }

   // Call threadExit() in parent class
   SchedulerPinnedBase::threadExit(thread_id, time);

   if (m_debug_output)
      printState();
}

void SchedulerBigSmall::periodic(SubsecondTime time)
{
   bool print_state = false;

   if (time > m_last_reshuffle + m_quantum)
   {
      // First move all threads back to the small cores
      for(thread_id_t thread_id = 0; thread_id < (thread_id_t)Sim()->getThreadManager()->getNumThreads(); ++thread_id)
      {
         if (m_thread_isbig[thread_id])
            moveToSmall(thread_id);
      }
      // Now promote as many threads to the big core pool as there are big cores
      for(UInt64 i = 0; i < std::min(m_num_big_cores, Sim()->getThreadManager()->getNumThreads()); ++i)
      {
         pickBigThread();
      }

      m_last_reshuffle = time;
      print_state = true;
   }

   // Call periodic() in parent class
   SchedulerPinnedBase::periodic(time);

   if (print_state && m_debug_output)
         printState();
}

void SchedulerBigSmall::moveToSmall(thread_id_t thread_id)
{
   threadSetAffinity(INVALID_THREAD_ID, thread_id, sizeof(m_mask_small), &m_mask_small);
   m_thread_isbig[thread_id] = false;
}

void SchedulerBigSmall::moveToBig(thread_id_t thread_id)
{
   threadSetAffinity(INVALID_THREAD_ID, thread_id, sizeof(m_mask_big), &m_mask_big);
   m_thread_isbig[thread_id] = true;
}

void SchedulerBigSmall::pickBigThread()
{
   // Randomly select one thread to promote from the small to the big core pool

   // First build a list of all eligible cores
   std::vector<thread_id_t> eligible;
   for(thread_id_t thread_id = 0; thread_id < (thread_id_t)Sim()->getThreadManager()->getNumThreads(); ++thread_id)
   {
      if (m_thread_isbig[thread_id] == false && m_threads_runnable[thread_id])
      {
         eligible.push_back(thread_id);
      }
   }

   if (eligible.size() > 0)
   {
      // Randomly select a thread from our list
      thread_id_t thread_id = eligible[rng_next(m_rng) % eligible.size()];
      moveToBig(thread_id);

      if (m_debug_output)
         std::cout << "[SchedulerBigSmall] thread " << thread_id << " promoted to big core" << std::endl;
   }
}
