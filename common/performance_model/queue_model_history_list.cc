#include "queue_model_history_list.h"
#include "simulator.h"
#include "core_manager.h"
#include "config.h"
#include "fxsupport.h"
#include "log.h"
#include "stats.h"
#include "config.hpp"

QueueModelHistoryList::QueueModelHistoryList(String name, UInt32 id, SubsecondTime min_processing_time):
   m_min_processing_time(min_processing_time),
   m_utilized_time(SubsecondTime::Zero()),
   m_total_queue_delay(SubsecondTime::Zero()),
   m_total_requests(0),
   m_total_requests_using_analytical_model(0)
{
   // history_list queuing model does not play nice with the interval core model:
   // The interval model issues memory operations in big batches, even when it later decides to serialize those,
   // The history_list queue model will in this case return large, increasing latencies which are then also serialized.
   // Also, this queuing model does not work very well with threads that are out-of-order.
   // In general, in a simulation environment with loose time synchronization (or fluffy time), you don't want
   // to look at exact times (such as this model does) since those are wrong, but use averages only (which are fine)
   // -- this is exactly what the Windowed M/G/1 queue model is supposed to do.
   //LOG_PRINT_WARNING_ONCE("history_list queuing model is deprecated. Please consider using windowed_mg1 instead.");

   // Some Hard-Coded values here
   // Assumptions
   // 1) Simulation Time will not exceed 2^63.
   UInt32 max_list_size = 0;
   try
   {
      m_analytical_model_enabled = Sim()->getCfg()->getBool("queue_model/history_list/analytical_model_enabled");
      max_list_size = Sim()->getCfg()->getInt("queue_model/history_list/max_list_size");
   }
   catch(...)
   {
      LOG_PRINT_ERROR("Could not read parameters from cfg");
   }
   m_max_free_interval_list_size = max_list_size;
   m_average_delay = MovingAverage<SubsecondTime>::createAvgType(MovingAverage<SubsecondTime>::ARITHMETIC_MEAN, max_list_size);
   SubsecondTime max_simulation_time = SubsecondTime::FS() << 63;
   m_free_interval_list.push_back(std::pair<const SubsecondTime,SubsecondTime>(SubsecondTime::Zero(), max_simulation_time));

   registerStatsMetric(name, id, "num-requests", &m_total_requests);
   registerStatsMetric(name, id, "num-requests-analytical", &m_total_requests_using_analytical_model);
   registerStatsMetric(name, id, "total-time-used", &m_utilized_time);
   registerStatsMetric(name, id, "total-queue-delay", &m_total_queue_delay);
}

QueueModelHistoryList::~QueueModelHistoryList()
{
   delete m_average_delay;
}

SubsecondTime
QueueModelHistoryList::computeQueueDelay(SubsecondTime pkt_time, SubsecondTime processing_time, core_id_t requester)
{
   LOG_ASSERT_ERROR(m_free_interval_list.size() >= 1,
         "Free Interval list size < 1");

   SubsecondTime queue_delay;

   // Check if it is an old packet
   // If yes, use analytical model
   // If not, use the history list based queue model
   std::pair<SubsecondTime,SubsecondTime> oldest_interval = m_free_interval_list.front();
   if (m_analytical_model_enabled && ((pkt_time + processing_time) <= oldest_interval.first))
   {
      // Increment the number of requests that use the analytical model
      m_total_requests_using_analytical_model ++;
      queue_delay = computeUsingAnalyticalModel(pkt_time, processing_time);
   }
   else
   {
      queue_delay = computeUsingHistoryList(pkt_time, processing_time);
      updateAverageDelay(queue_delay);
   }

   updateQueueUtilization(processing_time);

   // Increment total queue requests
   m_total_requests ++;
   m_total_queue_delay += queue_delay;

   return queue_delay;
}

float
QueueModelHistoryList::getQueueUtilization()
{
   std::pair<SubsecondTime,SubsecondTime> newest_interval = m_free_interval_list.back();
   SubsecondTime total_time = newest_interval.first;

   if (total_time == SubsecondTime::Zero())
   {
      LOG_ASSERT_ERROR(m_utilized_time == SubsecondTime::Zero(), "m_utilized_time(%s), total_time(%s)",
            itostr(m_utilized_time).c_str(), itostr(total_time).c_str());
      return 0;
   }
   else
   {
      return ((float) m_utilized_time.getInternalDataForced() / total_time.getInternalDataForced());
   }
}

float
QueueModelHistoryList::getFracRequestsUsingAnalyticalModel()
{
  if (m_total_requests == 0)
     return 0;
  else
     return ((float) m_total_requests_using_analytical_model / m_total_requests);
}

void
QueueModelHistoryList::updateQueueUtilization(SubsecondTime processing_time)
{
   // Update queue utilization parameter
   m_utilized_time += processing_time;
}

void
QueueModelHistoryList::updateAverageDelay(SubsecondTime queue_delay)
{
   m_average_delay->update(queue_delay);
}

SubsecondTime
QueueModelHistoryList::computeUsingAnalyticalModel(SubsecondTime pkt_time, SubsecondTime processing_time)
{
   // WH: Old code used general queuing model to estimate delay based on historical utilization rate:
   //         queue_delay = (rho * processing_time) / (2 * (1 - rho))) + 1
   //     For rho near or over 1, which does happen due to other approximations (skew between cores), this yields nonsense.
   //     Yet, everyone knows that in a system with feedback, the arrival rate is no longer an independent, exponential process
   //     (i.e., more delay causes upstream buffer blockages which throttle the actual request rate, avoiding rho from ever reaching 1)
   // Current best guess: return average of delays computed using history list model
   return m_average_delay->compute();
}

SubsecondTime
QueueModelHistoryList::computeUsingHistoryList(SubsecondTime pkt_time, SubsecondTime processing_time)
{
   LOG_ASSERT_ERROR(m_free_interval_list.size() <= m_max_free_interval_list_size,
         "Free Interval list size(%u) > %u", m_free_interval_list.size(), m_max_free_interval_list_size);
   SubsecondTime queue_delay = SubsecondTime::MaxTime();

   FreeIntervalList::iterator curr_it;
   for (curr_it = m_free_interval_list.begin(); curr_it != m_free_interval_list.end(); curr_it ++)
   {
      std::pair<SubsecondTime,SubsecondTime> interval = (*curr_it);

      if ((pkt_time >= interval.first) && ((pkt_time + processing_time) <= interval.second))
      {
         queue_delay = SubsecondTime::Zero();
         // Adjust the data structure accordingly
         curr_it = m_free_interval_list.erase(curr_it);
         if ((pkt_time - interval.first) >= m_min_processing_time)
         {
            m_free_interval_list.insert(curr_it, std::pair<SubsecondTime,SubsecondTime>(interval.first, pkt_time));
         }
         if ((interval.second - (pkt_time + processing_time)) >= m_min_processing_time)
         {
            m_free_interval_list.insert(curr_it, std::pair<SubsecondTime,SubsecondTime>(pkt_time + processing_time, interval.second));
         }
         break;
      }
      // WH: The request comes before this free part, but doesn't fit. It doesn't make sense to me to
      //     demand a fit and move this request down even further. In reality, this request would have most
      //     likely executed at interval.first, while later request would/could be delayed. But it's too late
      //     for that now.
      //     (If we assume all wait times are additive then the average works out by shifting it down,
      //      but since this is an interactive simulation all delays propagate through the system
      //      so this won't be accurate.)
      else if ((pkt_time < interval.first) /*&& ((interval.first + processing_time) <= interval.second)*/)
      {
         queue_delay = interval.first - pkt_time;
         // Adjust the data structure accordingly
         curr_it = m_free_interval_list.erase(curr_it);
         if ((interval.second - (interval.first + processing_time)) >= m_min_processing_time)
         {
            m_free_interval_list.insert(curr_it, std::pair<SubsecondTime,SubsecondTime>(interval.first + processing_time, interval.second));
         }
         break;
      }
   }

   LOG_ASSERT_ERROR(queue_delay != SubsecondTime::MaxTime(), "queue delay(%s), free interval not found", itostr(queue_delay).c_str());

   if (m_free_interval_list.size() > m_max_free_interval_list_size)
   {
      m_free_interval_list.erase(m_free_interval_list.begin());
   }

   LOG_PRINT("HistoryList: pkt_time(%s), processing_time(%s), queue_delay(%s)", itostr(pkt_time).c_str(), itostr(processing_time).c_str(), itostr(queue_delay).c_str());

   return queue_delay;
}
