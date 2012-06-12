#include "contention_model.h"
#include "stats.h"
#include "subsecond_time.h"
#include "dvfs_manager.h"

ContentionModel::ContentionModel()
   : m_num_outstanding(1)
   , m_time(m_num_outstanding, std::make_pair(SubsecondTime::Zero(), 0))
   , m_t_last(SubsecondTime::Zero())
   , m_proc_period(NULL)
   , m_n_requests(0)
   , m_n_barriers(0)
   , m_n_outoforder(0)
   , m_n_simultaneous(0)
   , m_n_hasfreefail(0)
   , m_total_delay(SubsecondTime::Zero())
   , m_total_barrier_delay(SubsecondTime::Zero())
{}

ContentionModel::ContentionModel(String name, core_id_t core_id, UInt32 num_outstanding)
   : m_num_outstanding(num_outstanding)
   , m_time(m_num_outstanding, std::make_pair(SubsecondTime::Zero(), 0))
   , m_t_last(SubsecondTime::Zero())
   , m_proc_period(Sim()->getDvfsManager()->getCoreDomain(core_id))
   , m_n_requests(0)
   , m_n_barriers(0)
   , m_n_outoforder(0)
   , m_n_simultaneous(0)
   , m_n_hasfreefail(0)
   , m_total_delay(SubsecondTime::Zero())
   , m_total_barrier_delay(SubsecondTime::Zero())
{
   if (m_num_outstanding > 0)
   {
      registerStatsMetric(name, core_id, "num-requests", &m_n_requests);
      registerStatsMetric(name, core_id, "num-barriers", &m_n_barriers);
      registerStatsMetric(name, core_id, "requests-out-of-order", &m_n_outoforder);
      registerStatsMetric(name, core_id, "requests-simultaneous", &m_n_simultaneous);
      registerStatsMetric(name, core_id, "no-free-slots", &m_n_hasfreefail);
      registerStatsMetric(name, core_id, "total-delay", &m_total_delay);
      registerStatsMetric(name, core_id, "total-barrier-delay", &m_total_barrier_delay);
   }
}

ContentionModel::~ContentionModel()
{}

UInt32
ContentionModel::getNumUsed(uint64_t t_start)
{
   SubsecondTimeCycleConverter conv(m_proc_period);
   return getNumUsed(conv.cyclesToSubsecondTime(t_start));
}

UInt32
ContentionModel::getNumUsed(SubsecondTime t_start)
{
   UInt32 num_used = 0;
   for (UInt32 i = 0; i < m_num_outstanding; ++i)
   {
      if (m_time[i].first > t_start)
         ++num_used;
   }
   return num_used;
}

SubsecondTime
ContentionModel::getTagCompletionTime(UInt64 tag)
{
   for (UInt32 i = 0; i < m_num_outstanding; ++i)
   {
      if (m_time[i].second == tag)
         return m_time[i].first;
   }
   return SubsecondTime::MaxTime();
}

bool
ContentionModel::hasFreeSlot(uint64_t t_start, UInt64 tag)
{
   SubsecondTimeCycleConverter conv(m_proc_period);
   return hasFreeSlot(conv.cyclesToSubsecondTime(t_start), tag);
}

bool
ContentionModel::hasFreeSlot(SubsecondTime t_start, UInt64 tag)
{
   for (UInt32 i = 0; i < m_num_outstanding; ++i)
   {
      if (m_time[i].first <= t_start)
         return true;

      // When using tags: an identical tag that's already in process is also acceptable
      if (m_time[i].second == tag)
         return true;
   }
   ++m_n_hasfreefail;
   return false;
}

bool
ContentionModel::hasTag(UInt64 tag)
{
   for (UInt32 i = 0; i < m_num_outstanding; ++i)
   {
      if (m_time[i].second == tag)
         return true;
   }
   return false;
}

uint64_t
ContentionModel::getBarrierCompletionTime(uint64_t t_start, uint64_t t_delay, UInt64 tag)
{
   SubsecondTimeCycleConverter conv(m_proc_period);
   SubsecondTime result = getBarrierCompletionTime(conv.cyclesToSubsecondTime(t_start),
                                                   conv.cyclesToSubsecondTime(t_delay),
                                                   tag);
   return conv.subsecondTimeToCycles(result);
}

SubsecondTime
ContentionModel::getBarrierCompletionTime(SubsecondTime t_start, SubsecondTime t_delay, UInt64 tag)
{
   if (m_num_outstanding == 0)
      return t_start + t_delay;

  SubsecondTime max_time = t_start;
  for (UInt32 i = 0; i < m_num_outstanding; ++i)
  {
    if (m_time[i].first > max_time)
      max_time = m_time[i].first;
  }

  for (UInt32 i = 0; i < m_num_outstanding; ++i)
  {
    m_time[i].first = max_time + t_delay;
    m_time[i].second = tag;
  }

  m_total_barrier_delay += max_time - t_start;
  ++m_n_barriers;

  return max_time + t_delay;
}

uint64_t
ContentionModel::getCompletionTime(uint64_t t_start, uint64_t t_delay, UInt64 tag)
{
   SubsecondTimeCycleConverter conv(m_proc_period);
   SubsecondTime result = getCompletionTime(conv.cyclesToSubsecondTime(t_start),
                                            conv.cyclesToSubsecondTime(t_delay),
                                            tag);
   return conv.subsecondTimeToCycles(result);
}

/* Model utilization. In: start time and utilization time. Out: completion time */
SubsecondTime
ContentionModel::getCompletionTime(SubsecondTime t_start, SubsecondTime t_delay, UInt64 tag)
{
   if (m_num_outstanding == 0)
      return t_start + t_delay;

   SubsecondTime t_end;

   if (t_start == SubsecondTime::Zero())
      t_end = t_delay;

   else if (t_start < m_t_last)
   {
      /* Out of order packet. Assume no congestion, only transfer latency. */
      t_end = t_start + t_delay;

      ++m_n_outoforder;

      #if 0
      /* Update time of last seen item */
      m_t_last = t_start;

      /* Reset all counters to start again from now */
      m_time[0].first = t_end;
      m_time[0].second = tag;
      for(UInt32 i = 1; i < m_num_outstanding; ++i)
      {
         m_time[i].first = SubsecondTime::Zero();
         m_time[i].second = 0;
      }
      #endif

   }
   else
   {
      if (t_start == m_t_last)
         m_n_simultaneous ++;

      UInt64 unit = 0;
      /* Find first free entry */
      for(UInt32 i = 0; i < m_num_outstanding; ++i)
      {
         if (m_time[i].first <= t_start)
         {
            /* This one is free now */
            unit = i;
            break;
         }
         else if (m_time[i].first < m_time[unit].first)
         {
            /* Unit i is the first one free */
            unit = i;
         }
      }

      SubsecondTime t_begin;
      if (t_start < m_time[unit].first)
         /* Delay until the time the first unit becomes free */
         t_begin = m_time[unit].first;
      else
         /* We only arrive after this unit became free */
         t_begin = t_start;
      /* Compute end of packet sending time */
      t_end = t_begin + t_delay;

      m_time[unit].first = t_end;
      m_time[unit].second = tag;

      /* Update statistics */
      m_total_delay += t_begin - t_start;

      /* Update time of last seen item */
      m_t_last = t_start;
   }

   ++m_n_requests;

   return t_end;
}

uint64_t
ContentionModel::getStartTime(uint64_t t_start)
{
   SubsecondTimeCycleConverter conv(m_proc_period);
   SubsecondTime result = getStartTime(conv.cyclesToSubsecondTime(t_start));
   return conv.subsecondTimeToCycles(result);
}

SubsecondTime
ContentionModel::getStartTime(SubsecondTime t_start)
{
   /* Peek start time for a new request */

   if (m_num_outstanding == 0)
      return t_start;

   if (t_start < m_t_last)
   {
      // Out-of-order: start time will be instantly
      return t_start;
   }
   else
   {
      UInt64 unit = 0;
      /* Find first free entry */
      for(UInt32 i = 0; i < m_num_outstanding; ++i)
      {
         if (m_time[i].first <= t_start)
         {
            /* This one is free now */
            return t_start;
         }
         else if (m_time[i].first < m_time[unit].first)
         {
            /* Unit i is the first one free */
            unit = i;
         }
      }

      if (t_start < m_time[unit].first)
         /* Delay until the time the first unit becomes free */
         return m_time[unit].first;
      else
         /* We only arrive after this unit became free */
         return t_start;
   }
}
