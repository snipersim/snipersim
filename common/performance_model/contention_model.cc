#include "contention_model.h"
#include "stats.h"
#include "subsecond_time.h"
#include "dvfs_manager.h"

ContentionModel::ContentionModel()
   : m_num_outstanding(1)
   , m_time(m_num_outstanding, SubsecondTime::Zero())
   , m_t_last(SubsecondTime::Zero())
   , m_proc_period(NULL)
   , m_n_requests(0)
   , m_n_barriers(0)
   , m_n_outoforder(0)
   , m_n_simultaneous(0)
   , m_total_delay(SubsecondTime::Zero())
   , m_total_barrier_delay(SubsecondTime::Zero())
{}

ContentionModel::ContentionModel(String name, core_id_t core_id, UInt32 num_outstanding)
   : m_num_outstanding(num_outstanding)
   , m_time(m_num_outstanding, SubsecondTime::Zero())
   , m_t_last(SubsecondTime::Zero())
   , m_proc_period(Sim()->getDvfsManager()->getCoreDomain(core_id))
   , m_n_requests(0)
   , m_n_barriers(0)
   , m_n_outoforder(0)
   , m_n_simultaneous(0)
   , m_total_delay(SubsecondTime::Zero())
   , m_total_barrier_delay(SubsecondTime::Zero())
{
   if (m_num_outstanding > 0) {
      registerStatsMetric(name, core_id, "num-requests", &m_n_requests);
      registerStatsMetric(name, core_id, "num-barriers", &m_n_barriers);
      registerStatsMetric(name, core_id, "requests-out-of-order", &m_n_outoforder);
      registerStatsMetric(name, core_id, "requests-simultaneous", &m_n_simultaneous);
      registerStatsMetric(name, core_id, "total-delay", &m_total_delay);
      registerStatsMetric(name, core_id, "total-barrier-delay", &m_total_barrier_delay);
   }
}

ContentionModel::~ContentionModel()
{}

uint64_t
ContentionModel::getBarrierCompletionTime(uint64_t t_start, uint64_t t_delay)
{
    SubsecondTimeCycleConverter conv(m_proc_period);
	SubsecondTime result = getBarrierCompletionTime(conv.cyclesToSubsecondTime(t_start),
	                                                conv.cyclesToSubsecondTime(t_delay));
    return conv.subsecondTimeToCycles(result);
}

SubsecondTime
ContentionModel::getBarrierCompletionTime(SubsecondTime t_start, SubsecondTime t_delay)
{
  SubsecondTime max_time = t_start;
  for (UInt32 i = 0; i < m_num_outstanding; ++i)
  {
    if (m_time[i] > max_time)
      max_time = m_time[i];
  }

  for (UInt32 i = 0; i < m_num_outstanding; ++i)
  {
    m_time[i] = max_time + t_delay;
  }

  m_total_barrier_delay += max_time - t_start;
  ++m_n_barriers;

  return max_time + t_delay;
}

uint64_t
ContentionModel::getCompletionTime(uint64_t t_start, uint64_t t_delay)
{
    SubsecondTimeCycleConverter conv(m_proc_period);
	SubsecondTime result = getCompletionTime(conv.cyclesToSubsecondTime(t_start),
	                                                conv.cyclesToSubsecondTime(t_delay));
    return conv.subsecondTimeToCycles(result);
}

/* Model utilization. In: start time and utilization time. Out: completion time */
SubsecondTime
ContentionModel::getCompletionTime(SubsecondTime t_start, SubsecondTime t_delay)
{
   if (m_num_outstanding == 0)
      return t_start + t_delay;

   SubsecondTime t_end;

   if (t_start == SubsecondTime::Zero())
      t_end = t_delay;

   else if (t_start < m_t_last) {
      /* Out of order packet. Assume no congestion, only transfer latency. */
      t_end = t_start + t_delay;

      ++m_n_outoforder;

      #if 0
      /* Update time of last seen item */
      m_t_last = t_start;

      /* Reset all counters to start again from now */
      m_time[0] = t_end;
      for(UInt32 i = 1; i < m_num_outstanding; ++i)
         m_time[i] = SubsecondTime::Zero();
      #endif

   } else {
      if (t_start == m_t_last)
         m_n_simultaneous ++;

      UInt64 unit = 0;
      /* Find first free entry */
      for(UInt32 i = 0; i < m_num_outstanding; ++i) {
         if (m_time[i] <= t_start) {
            /* This one is free now */
            unit = i;
            break;
         } else if (m_time[i] < m_time[unit]) {
            /* Unit i is the first one free */
            unit = i;
         }
      }

      SubsecondTime t_begin;
      if (t_start < m_time[unit])
         /* Delay until the time the first unit becomes free */
         t_begin = m_time[unit];
      else
         /* We only arrive after this unit became free */
         t_begin = t_start;
      /* Compute end of packet sending time */
      t_end = t_begin + t_delay;

      m_time[unit] = t_end;

      /* Update statistics */
      m_total_delay += t_begin - t_start;

      /* Update time of last seen item */
      m_t_last = t_start;
   }

   ++m_n_requests;

   return t_end;
}

