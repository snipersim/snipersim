#include "scheduler_roaming.h"
#include "config.hpp"

SchedulerRoaming::SchedulerRoaming(ThreadManager *thread_manager)
   : SchedulerPinnedBase(thread_manager, SubsecondTime::NS(Sim()->getCfg()->getInt("scheduler/roaming/quantum")))
{
   m_core_mask.resize(Sim()->getConfig()->getApplicationCores());

   for (core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); core_id++)
   {
       m_core_mask[core_id] = Sim()->getCfg()->getBoolArray("scheduler/roaming/core_mask", core_id);
   }
}

void SchedulerRoaming::threadSetInitialAffinity(thread_id_t thread_id)
{
   m_thread_info[thread_id].clearAffinity();

   for(core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
   {
      if (m_core_mask[core_id])
         m_thread_info[thread_id].addAffinity(core_id);
   }
}
