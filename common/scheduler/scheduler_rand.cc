#include "scheduler_rand.h"
#include "simulator.h"
#include "config.hpp"
#include "thread.h"
#include "performance_model.h"
#include "core_manager.h"
#include <list>

SchedulerRand::SchedulerRand(ThreadManager *thread_manager)
   : SchedulerDynamic(thread_manager)
   , m_quantum(SubsecondTime::US(1000))
   , m_last_periodic(SubsecondTime::Zero())
{
   std::cout<< "created random scheduler" << std::endl;

   uint64_t t = Sim()->getCfg()->getInt("scheduler/quantum");
   m_quantum = SubsecondTime::US(t);

   // Small and Big cores can be defined to be anything, using tags to identify them.
   m_nSmallCores = m_nBigCores= 0;
   for (core_id_t coreId = 0; coreId < (core_id_t) Sim()->getConfig()->getApplicationCores(); coreId++)
   {
      bool inOrder = Sim()->getCfg()->getBoolArray("perf_model/core/rob_timer/in_order", coreId); 
      if (inOrder) m_nSmallCores++; else m_nBigCores++;
   }
}

core_id_t SchedulerRand::threadCreate(thread_id_t thread_id)
{
   // initially schedule threads on the next available core
   core_id_t freeCoreId = findFirstFreeCore();
   LOG_ASSERT_ERROR(freeCoreId != INVALID_CORE_ID, "[scheduler] No cores available for spawnThread request.");

   m_coreToThreadMapping.insert( std::pair<core_id_t,thread_id_t>( freeCoreId, thread_id ));

   m_quantum_left = m_quantum;

   return freeCoreId;
}

void SchedulerRand::threadStart(thread_id_t thread_id, SubsecondTime time)
{
   // Nothing needs to be done
}

void SchedulerRand::threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time)
{
   // currently unsupported: thread will just waste a core
}

void SchedulerRand::threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time)
{
   // Nothing needs to be done: stalling is not supported
}

void SchedulerRand::threadExit(thread_id_t thread_id, SubsecondTime time)
{
   // currently unsupported: thread will just waste a core
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
   // For any scheduler, it is generally a good idea to randomize the core order to avoid 
   // biasing due to original thread-to-core mapping.
   std::list< std::pair< uint64_t, core_id_t  > > cid;
   for (core_id_t coreId = 0; coreId < (core_id_t) Sim()->getConfig()->getApplicationCores(); coreId++)
   {
      cid.push_back( std::pair< uint64_t, core_id_t >( rand() , coreId) );
      
      m_threads_stats[ m_coreToThreadMapping[coreId] ]->update(time);

   }
   // Sorting using a random value generates a random ordering.
   cid.sort();

   // Now sort all threads based on some optimization criterium
   // note: for this random scheduler, this value is, euhm, random.
   std::list< std::pair< uint64_t, core_id_t> > mapping;
   for( std::list< std::pair< uint64_t, core_id_t> >::iterator it= cid.begin(); it != cid.end(); ++it)
   {
      core_id_t coreId = it->second;
      uint64_t someValue = rand();
      mapping.push_back( std::pair< uint64_t, core_id_t >( someValue, coreId  ) );
   }
   mapping.sort();

   // The threads with the lowest values get scheduled on the small cores, the remaining ones
   // end up on the big cores.
   std::list< std::pair< uint64_t , core_id_t> >::reverse_iterator rit= mapping.rbegin();
   std::list< std::pair< uint64_t , core_id_t> >::iterator it= mapping.begin();
   for( uint64_t i=0; i < m_nSmallCores ; i++)
   {  
      core_id_t coreId = it->second;
      bool inOrder = Sim()->getCfg()->getBoolArray("perf_model/core/rob_timer/in_order", coreId); 

      if (inOrder)
      {
         // This thread needs to end up on a small core (low value), but already is; don't move it.
         it++;
         continue;
      }
      else
      {
         // This thread has a low enough value so that it should end up on a small core, but it 
         // is currently running on a big core. Find a suitable candidate to swap with: the thread
         // with the highest value that is currently scheduled on a big core type.
         while ( Sim()->getCfg()->getBoolArray("perf_model/core/rob_timer/in_order", rit->second ) != 1 )
         {
            rit++;
         }
         
         // Move thread on the big core to a temp core (invalid core).
         moveThread( m_coreToThreadMapping[rit->second] , INVALID_CORE_ID, time);
         // Move thread on the small core to the big core.
         moveThread( m_coreToThreadMapping[it->second] , rit->second, time);
         // Move thread from the temp core to the small core.
         moveThread( m_coreToThreadMapping[rit->second] , it->second, time);

         uint64_t tmp = m_coreToThreadMapping[it->second];
         m_coreToThreadMapping[it->second] = m_coreToThreadMapping[rit->second];
         m_coreToThreadMapping[rit->second] = tmp;

         rit++;
         it++;
      }
   }

   // Print mapping
   printf("mapping: ");
   for(uint64_t i = 0; i < Sim()->getConfig()->getApplicationCores(); ++i)
   {
      std::cout << "( c"<< i << "::t" << Sim()->getThreadManager()->getThreadFromID(i)->getCore()->getId() << ") ";
   } 
   std::cout << std::endl;

   m_quantum_left = m_quantum;
}
