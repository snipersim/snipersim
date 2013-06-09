#include "scheduler_pinned.h"
#include "config.hpp"

SchedulerPinned::SchedulerPinned(ThreadManager *thread_manager)
   : SchedulerPinnedBase(thread_manager, SubsecondTime::NS(Sim()->getCfg()->getInt("scheduler/pinned/quantum")))
   , m_interleaving(Sim()->getCfg()->getInt("scheduler/pinned/interleaving"))
   , m_next_core(0)
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
   if (core_id >= (core_id_t)Sim()->getConfig()->getApplicationCores())
   {
      core_id %= Sim()->getConfig()->getApplicationCores();
      core_id += 1;
      core_id %= m_interleaving;
   }
   return core_id;
}

void SchedulerPinned::threadSetInitialAffinity(thread_id_t thread_id)
{
   while(!m_core_mask[m_next_core])
      m_next_core = getNextCore(m_next_core);

   core_id_t core_id = m_next_core;
   m_thread_info[thread_id].setAffinitySingle(core_id);

   m_next_core = getNextCore(m_next_core);
}
