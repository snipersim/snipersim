#include "barrier_sync_client.h"
#include "simulator.h"
#include "config.h"
#include "core.h"
#include "performance_model.h"
#include "subsecond_time.h"
#include "dvfs_manager.h"
#include "config.hpp"

BarrierSyncClient::BarrierSyncClient(Core* core):
   m_core(core),
   m_num_outstanding(0)
{
   try
   {
      m_barrier_interval = SubsecondTime::NS() * Sim()->getCfg()->getInt("clock_skew_minimization/barrier/quantum");
   }
   catch(...)
   {
      LOG_PRINT_ERROR("Error Reading 'clock_skew_minimization/barrier/quantum' from the config file");
   }
   m_next_sync_time = m_barrier_interval;
}

BarrierSyncClient::~BarrierSyncClient()
{}

void
BarrierSyncClient::synchronize(SubsecondTime time, bool ignore_time, bool abort_func(void*), void* abort_arg)
{
   SubsecondTime curr_elapsed_time = time;
   if (time == SubsecondTime::Zero())
      curr_elapsed_time = m_core->getPerformanceModel()->getElapsedTime();

   if (curr_elapsed_time >= m_next_sync_time || ignore_time)
   {
      // TODO: implement interruptable wait

      Sim()->getClockSkewMinimizationServer()->synchronize(m_core->getId(), curr_elapsed_time);

      // Update barrier interval in case it was changed
      m_barrier_interval = Sim()->getClockSkewMinimizationServer()->getBarrierInterval();
      // Update 'm_next_sync_time'
      m_next_sync_time = ((curr_elapsed_time / m_barrier_interval) * m_barrier_interval) + m_barrier_interval;
   }
}
