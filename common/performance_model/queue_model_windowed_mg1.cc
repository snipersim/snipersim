#include "queue_model_windowed_mg1.h"
#include "simulator.h"
#include "config.hpp"
#include "log.h"

QueueModelWindowedMG1::QueueModelWindowedMG1(String name, UInt32 id)
   : m_window_size(SubsecondTime::NS(Sim()->getCfg()->getInt("queue_model/windowed_mg1/window_size")))
{}

QueueModelWindowedMG1::~QueueModelWindowedMG1()
{}

SubsecondTime
QueueModelWindowedMG1::computeQueueDelay(SubsecondTime pkt_time, SubsecondTime processing_time, core_id_t requester)
{
   SubsecondTime t_queue = SubsecondTime::Zero();

   removeItems(pkt_time - m_window_size);

   if (m_num_arrivals > 1)
   {
      double utilization = (double)m_service_time_sum / m_window_size.getPS();
      double arrival_rate = (double)m_num_arrivals / m_window_size.getPS();

      double service_time_Es2 = m_service_time_sum2 / m_num_arrivals;

      // If requesters do not throttle based on returned latency, it's their problem, not ours
      if (utilization > .99)
         utilization = .99;

      t_queue = SubsecondTime::PS(arrival_rate * service_time_Es2 / (2 * (1. - utilization)));

      // Our memory is limited in time to m_window_size. It would be strange to return more latency than that.
      if (t_queue > m_window_size)
         t_queue = m_window_size;
   }

   addItem(pkt_time, processing_time);

   return t_queue;
}

void
QueueModelWindowedMG1::addItem(SubsecondTime pkt_time, SubsecondTime service_time)
{
   m_window.insert(std::pair<SubsecondTime, SubsecondTime>(pkt_time, service_time));
   m_num_arrivals ++;
   m_service_time_sum += service_time.getPS();
   m_service_time_sum2 += service_time.getPS() * service_time.getPS();
}

void
QueueModelWindowedMG1::removeItems(SubsecondTime earliest_time)
{
   while(!m_window.empty() && m_window.begin()->first < earliest_time)
   {
      std::multimap<SubsecondTime, SubsecondTime>::iterator entry = m_window.begin();
      m_num_arrivals --;
      m_service_time_sum -= entry->second.getPS();
      m_service_time_sum2 -= entry->second.getPS() * entry->second.getPS();
      m_window.erase(entry);
   }
}
