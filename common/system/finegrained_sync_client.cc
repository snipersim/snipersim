#include <cassert>

#include "finegrained_sync_client.h"
#include "simulator.h"
#include "config.h"
#include "core.h"
#include "performance_model.h"
#include "thread_manager.h"
#include "dvfs_manager.h"
#include "config.hpp"


FinegrainedSyncClient::SyncerStruct* FinegrainedSyncClient::t_last = NULL;


FinegrainedSyncClient::FinegrainedSyncClient(Core* core):
   m_core(core),
   m_skew(ComponentLatency(NULL,0))
{
   try
   {
      m_skew = ComponentLatency(Sim()->getDvfsManager()->getCoreDomain(m_core->getId()),Sim()->getCfg()->getInt("clock_skew_minimization/finegrained/quantum"));
   }
   catch(...)
   {
      LOG_PRINT_ERROR("Error Reading 'clock_skew_minimization/finegrained/quantum' from the config file");
   }
   if ((m_skew.getLatency() != SubsecondTime::Zero()) && !t_last)
      t_last = new SyncerStruct[Sim()->getConfig()->getApplicationCores()];
}

FinegrainedSyncClient::~FinegrainedSyncClient()
{
   if (t_last) {
      delete t_last;
      t_last = NULL;
   }
}

void
FinegrainedSyncClient::synchronize(SubsecondTime time, bool ignore_time, bool abort_func(void*), void* abort_arg)
{
   if (m_skew.getLatency() != SubsecondTime::Zero()) {
      core_id_t core_id = m_core->getId();

      /* Only do on application cores */
      if (core_id >= (core_id_t) Sim()->getConfig()->getApplicationCores())
         return;

      SubsecondTime curr_cycle_count = time;
      if (time == SubsecondTime::Zero())
         curr_cycle_count = m_core->getPerformanceModel()->getElapsedTime();

      /* Set our own time */
      t_last[core_id].time = curr_cycle_count;

      for(core_id_t i = 0; i < (core_id_t) Sim()->getConfig()->getApplicationCores(); ++i) {
         if (i != core_id) {
            while(1) {
               if (t_last[i].time + m_skew.getLatency() >= curr_cycle_count)
                  break;
               if (! Sim()->getThreadManager()->isThreadRunning(i))
                  break;
            }
         }
      }
   }
}
