#include "scheduler_rand.h"
#include "simulator.h"
#include "config.hpp"
#include "thread.h"
#include "performance_model.h"
#include "core_manager.h"
#include "misc/tags.h"
#include <list>

SchedulerRand::SchedulerRand(ThreadManager *thread_manager)
   : SchedulerDynamic(thread_manager)
   , m_quantum(SubsecondTime::US(1000))
   , m_last_periodic(SubsecondTime::Zero())
{
   std::cout<< "[scheduler] created random scheduler" << std::endl;

   uint64_t t = Sim()->getCfg()->getInt("scheduler/quantum");
   m_quantum = SubsecondTime::US(t);

   m_nSmallCores = m_nBigCores = 0;
   for (core_id_t coreId = 0; coreId < (core_id_t) Sim()->getConfig()->getApplicationCores(); coreId++)
   {
      bool isBig = Sim()->getTagsManager()->hasTag("core", coreId, "big");
      if (isBig) m_nBigCores++; else m_nSmallCores++;
   }
}


core_id_t SchedulerRand::threadCreate(thread_id_t thread_id)
{
   // initially schedule threads on the next available core
   core_id_t freeCoreId = findFirstFreeCore();
   LOG_ASSERT_ERROR(freeCoreId != INVALID_CORE_ID, "[scheduler] No cores available for spawnThread request.");

   std::cout << "[scheduler] created thread "<<  thread_id << " on core " << freeCoreId << " in randomscheduler" << std::endl;
   return freeCoreId;
}

void SchedulerRand::threadStart(thread_id_t thread_id, SubsecondTime time)
{
   std::cout << "[scheduler] thread "<< thread_id << " started" << std::endl;

   // Nothing needs to be done
}

void SchedulerRand::threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time)
{
   std::cout << "[scheduler] thread "<< thread_id << " stalled" << std::endl;
   // thread will just waste a core
}

void SchedulerRand::threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time)
{
   std::cout << "[scheduler] thread "<< thread_id << " resumed" << std::endl;
   // Nothing needs to be done
}

void SchedulerRand::threadExit(thread_id_t thread_id, SubsecondTime time)
{
   std::cout << "[scheduler] thread "<< thread_id << " EXIT" << std::endl;
   // Nothing needs to be done
}

void SchedulerRand::periodic(SubsecondTime time)
{
   SubsecondTime delta = time - m_last_periodic;

   if (delta > m_quantum_left)
   {
      reschedule(time);
   }
   else
   {
      m_quantum_left -= delta;
   }

   m_last_periodic = time;
}

void SchedulerRand::reschedule(SubsecondTime time)
{
   std::list< std::pair< uint64_t, core_id_t  > > cid;
   for (core_id_t coreId = 0; coreId < (core_id_t) Sim()->getConfig()->getApplicationCores(); coreId++)
   {
      cid.push_back( std::pair< uint64_t, core_id_t >( rand() , coreId) );

      if (  Sim()->getCoreManager()->getCoreFromID(coreId)->getState() != Core::IDLE  )
      {
         thread_id_t threadId = Sim()->getCoreManager()->getCoreFromID(coreId)->getThread()->getId();
         m_threads_stats[ threadId ]->update(time);
      }
   }
   // Sorting using a random value generates a random ordering.
   cid.sort();

   remap(cid, time);
   
   std::cout << "[scheduler] mapping: " ; printMapping();

   m_quantum_left = m_quantum;
}


void SchedulerRand::remap(std::list< std::pair< uint64_t, core_id_t> > &mapping, SubsecondTime time )
{
   // The threads with the lowest value get scheduled on the small cores, the remaining ones
   // end up on the big cores.
   std::list< std::pair< uint64_t , core_id_t> >::iterator it= mapping.begin();
   std::list< std::pair< uint64_t , core_id_t> >::iterator rit= mapping.end(); rit--;
   
   uint64_t left = 0;
   uint64_t right = mapping.size()-1;

   while ( left < right )
   {
      core_id_t coreId = it->second;

      bool isSmall = Sim()->getTagsManager()->hasTag("core", coreId, "small");
      if (isSmall)
      {
         // This thread needs to end up on a small core (low value), but already is; don't move it.
         it++; left++;
         continue;
      }
      else
      {
         // This thread has a low enough value so that it should end up on a small core, but it
         // is currently running on a big core. Find a suitable candidate to swap with: the thread
         // with the highest value that is currently scheduled on a big core type.
         while ( Sim()->getTagsManager()->hasTag("core", rit->second, "big") && (left < right) )  
         {
            rit--; right--;
         }
        
         if ( left != right )
         {
            core_id_t srcId = it->second;
            core_id_t destId = rit->second;
            Core::State srcState = Sim()->getCoreManager()->getCoreFromID(srcId)->getState();
            Core::State destState = Sim()->getCoreManager()->getCoreFromID(destId)->getState();

            if ((srcState == Core::INITIALIZING) || (destState == Core::INITIALIZING))
            {
               std::cout << "[scheduler] Will not move thread that is initializing" <<std::endl;
            }
            else
            {

               if (( srcState == Core::IDLE ) && ( destState != Core::IDLE ))
               {
                  thread_id_t threadId = Sim()->getCoreManager()->getCoreFromID(destId)->getThread()->getId();
                  moveThread( threadId , srcId, time);
               }
               else if (( srcState != Core::IDLE ) && ( destState == Core::IDLE ))
               {
                  thread_id_t threadId = Sim()->getCoreManager()->getCoreFromID(srcId)->getThread()->getId();
                  moveThread( threadId , destId, time);
               }
               else if  (( srcState != Core::IDLE ) && ( destState != Core::IDLE ))
               {
                  thread_id_t srcThreadId = Sim()->getCoreManager()->getCoreFromID(srcId)->getThread()->getId();
                  thread_id_t destThreadId = Sim()->getCoreManager()->getCoreFromID(destId)->getThread()->getId();

                  moveThread( destThreadId, INVALID_CORE_ID, time);
                  moveThread( srcThreadId , destId, time);
                  moveThread( destThreadId, srcId, time);
               }
            }
         }
         rit--; right--;
         it++; left++;
      }
   }
}
