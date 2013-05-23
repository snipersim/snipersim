#include "periodic_sampling.h"
#include "sampling_manager.h"
#include "simulator.h"
#include "core_manager.h"
#include "performance_model.h"
#include "fastforward_performance_model.h"
#include "config.hpp"
#include "average.h"


PeriodicSampling::PeriodicSampling(SamplingManager *sampling_manager)
   : SamplingAlgorithm(sampling_manager)
   // Duration of a detailed interval
   , m_detailed_interval(SubsecondTime::NS(Sim()->getCfg()->getInt("sampling/periodic/detailed_interval")))
   // Duration of a fast-forward interval
   , m_fastforward_interval(SubsecondTime::NS(Sim()->getCfg()->getInt("sampling/periodic/fastforward_interval")))
   // Time between core synchronizations in fast-forward mode
   , m_fastforward_sync_interval(SubsecondTime::NS(Sim()->getCfg()->getInt("sampling/periodic/fastforward_sync_interval")))
   // Duration of a cache warmup interval
   , m_warmup_interval(SubsecondTime::NS(Sim()->getCfg()->getInt("sampling/periodic/warmup_interval")))
   , m_detailed_warmup_interval(SubsecondTime::NS(Sim()->getCfg()->getInt("sampling/periodic/detailed_warmup_interval")))
   // Whether to simulate synchronization during fast-forward (true, our method), or fast-forward using a per-core CPI that contains sync (false, COTSon method)
   , m_detailed_sync(Sim()->getCfg()->getBool("sampling/periodic/detailed_sync"))
   // Whether to randomly place the detailed interval inside the cycle (default: at the start)
   , m_random_placement(Sim()->getCfg()->getBool("sampling/periodic/random_placement"))
   , m_random_offset(SubsecondTime::Zero())
   , m_random_start(Sim()->getCfg()->getBool("sampling/periodic/random_start"))
   , m_periodic_last(SubsecondTime::Zero())
   , m_historic_cpi_intervals(Sim()->getConfig()->getApplicationCores(), NULL)
   , m_dispatch_width(Sim()->getCfg()->getInt("perf_model/core/interval_timer/dispatch_width"))
{
   LOG_ASSERT_ERROR(m_fastforward_sync_interval > SubsecondTime::Zero() && m_fastforward_sync_interval <= std::max(m_fastforward_interval, m_warmup_interval), "fastforward_sync_interval must be between 0 and max(fastforward_interval, warmup_interval)");

   UInt32 num_intervals = Sim()->getCfg()->getInt("sampling/periodic/num_historic_cpi_intervals");
   LOG_ASSERT_ERROR(num_intervals != 0, "Expected num_intervals to be >= 1");
   for (uint32_t i = 0 ; i < Sim()->getConfig()->getApplicationCores() ; i++)
      m_historic_cpi_intervals[i] = new CircularQueue<SubsecondTime>(num_intervals);

   if (Sim()->getCfg()->getBool("sampling/periodic/oneipc")) {
      m_constant_ipc = true;
      m_constant_ipcs.resize(Sim()->getConfig()->getApplicationCores());
      for (uint32_t i = 0 ; i < Sim()->getConfig()->getApplicationCores() ; i++)
         m_constant_ipcs[i] = 1.0;
   } else if (Sim()->getCfg()->getBool("sampling/periodic/xipc_enabled")) {
      m_constant_ipc = true;
      m_constant_ipcs.resize(Sim()->getConfig()->getApplicationCores());
      for (uint32_t i = 0 ; i < Sim()->getConfig()->getApplicationCores() ; i++)
         m_constant_ipcs[i] = Sim()->getCfg()->getFloatArray("sampling/periodic/xipcs",i);
   } else {
      m_constant_ipc = false;
   }

   if (m_random_placement || m_random_start)
   {
      UInt64 seed = Sim()->getCfg()->getInt("sampling/periodic/random_placement_seed");
      if (seed == 0)
         seed = time(NULL);
      m_prng.seed(seed);
   }

   if (m_random_start)
      m_random_offset = (m_fastforward_interval + m_warmup_interval) * (m_prng.next() % 100) / 100;
}

void
PeriodicSampling::callbackDetailed(SubsecondTime time)
{
   if (m_random_start)
   {
      // First interval: do some extra detailed (but ignore it's IPC) to offset the sampling intervals
      if (time < m_random_offset)
         ; // Stay in detailed
      else {
         printf("Done initial detailed of %" PRId64 " ns, now starting detailed interval for real\n", m_random_offset.getNS());
         m_sampling_manager->resetCoreHistoricCPIs();
         m_periodic_last = time;
         m_random_start = false;
      }
   }
   else if (time > m_periodic_last + m_detailed_interval)
   {
      LOG_ASSERT_ERROR(m_detailed_warmup_time_remaining == SubsecondTime::Zero(), "Should not finish detailed simulation before detailed warmup is complete.")
      //printf("IPC =");
      for(unsigned int core_id = 0; core_id < Sim()->getConfig()->getApplicationCores(); ++core_id)
      {
         Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
         SubsecondTime period = core->getDvfsDomain()->getPeriod();
         SubsecondTime cpi;
         if (m_constant_ipc)
         {
            cpi = (1.0/m_constant_ipcs[core_id]) * period;
         }
         else
         {
            SubsecondTime historic_cpi = m_sampling_manager->getCoreHistoricCPI(core, m_detailed_sync, m_detailed_interval / 5);
            // Only add to the history if we have been executing instructions for at least 20% of the time
            if (historic_cpi != SubsecondTime::Zero() && historic_cpi != SubsecondTime::MaxTime())
            {
               m_historic_cpi_intervals[core_id]->pushCircular(historic_cpi);
            }
            // If not empty, use the historic cpi information
            // If it is empty, assume one-ipc
            if (!m_historic_cpi_intervals[core_id]->empty())
               cpi = arithmetic_mean(*m_historic_cpi_intervals[core_id]);
            else
               cpi = period;

            SubsecondTime min_cpi = period / m_dispatch_width;
            if (cpi < min_cpi)
               cpi = min_cpi; // max. m_dispatch_width IPC
            else if (cpi > period * 100)
               cpi = period * 100; // min. .01 IPC
         }
         //printf(" %5.3f", float(period.getInternalDataForced()) / float(cpi.getInternalDataForced()));
         core->getPerformanceModel()->getFastforwardPerformanceModel()->setCurrentCPI(cpi);
      }
      //printf("\n");

      if (m_random_placement) {
         // |FFFFFFWWWDFFFFFF|FFFFWWWDFFFFFFFF|
         //            ^^^^^^ ^^^^=new offset
         //                 |= ffwd + warmup - old offset
         // Add whatever fast-forward we have left from the previous cycle
         m_fastforward_time_remaining = m_fastforward_interval + m_warmup_interval - m_random_offset;
         // Generate a new offset for this cycle
         m_random_offset = (m_prng.next() * m_fastforward_sync_interval) % (m_fastforward_interval + m_warmup_interval);
         // Fast-forward until the new offset
         m_fastforward_time_remaining += m_random_offset;
      } else {
         m_fastforward_time_remaining = m_fastforward_interval;
      }

      m_warmup_time_remaining = m_warmup_interval;
      bool done = stepFastForward(time, false);
      LOG_ASSERT_ERROR(done == false, "No fastforwarding to be done");
      m_periodic_last = time;
   }
   else if (m_detailed_warmup_time_remaining > SubsecondTime::Zero())
   {
      // If detailed warmup is enabled and we have simulated the requested detailed amount, reset the statistics and continue in detailed
      if (time > m_periodic_last + m_detailed_warmup_interval)
      {
         m_sampling_manager->resetCoreHistoricCPIs();
         m_detailed_warmup_time_remaining = SubsecondTime::Zero();
      }
   }
}

bool
PeriodicSampling::stepFastForward(SubsecondTime time, bool in_warmup)
{
   if (m_fastforward_time_remaining > SubsecondTime::Zero())
   {
      SubsecondTime time_to_fastforward = std::min(m_fastforward_time_remaining, m_fastforward_sync_interval);
      if (m_fastforward_time_remaining < time_to_fastforward)
        m_fastforward_time_remaining = SubsecondTime::Zero();
      else
        m_fastforward_time_remaining -= time_to_fastforward;
      m_sampling_manager->enableFastForward(time + time_to_fastforward, false, m_detailed_sync);
      return false;
   }
   else if (m_warmup_time_remaining > SubsecondTime::Zero())
   {
      SubsecondTime time_to_warmup = std::min(m_warmup_time_remaining, m_fastforward_sync_interval);
      if (m_warmup_time_remaining < time_to_warmup)
        m_warmup_time_remaining = SubsecondTime::Zero();
      else
        m_warmup_time_remaining -= time_to_warmup;
      m_sampling_manager->enableFastForward(time + time_to_warmup, true, m_detailed_sync);
      return false;
   }
   else
   {
      return true;
   }
}

void
PeriodicSampling::callbackFastForward(SubsecondTime time, bool in_warmup)
{
   bool done = stepFastForward(time, in_warmup);
   if (done)
   {
      m_sampling_manager->resetCoreHistoricCPIs();
      m_sampling_manager->disableFastForward();
      m_periodic_last = time;
      if (m_detailed_warmup_interval > SubsecondTime::Zero())
      {
         m_detailed_warmup_time_remaining = m_detailed_warmup_interval;
      }
   }
}
