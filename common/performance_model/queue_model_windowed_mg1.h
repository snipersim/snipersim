#ifndef __QUEUE_MODEL_WINDOWED_MG1_H__
#define __QUEUE_MODEL_WINDOWED_MG1_H__

#include "queue_model.h"
#include "fixed_types.h"
#include "contention_model.h"

#include <map>

class QueueModelWindowedMG1 : public QueueModel
{
public:
   QueueModelWindowedMG1(String name, UInt32 id);
   ~QueueModelWindowedMG1();

   SubsecondTime computeQueueDelay(SubsecondTime pkt_time, SubsecondTime processing_time, core_id_t requester = INVALID_CORE_ID);

private:
   const SubsecondTime m_window_size;

   UInt64 m_total_requests;
   SubsecondTime m_total_utilized_time;
   SubsecondTime m_total_queue_delay;

   std::multimap<SubsecondTime, SubsecondTime> m_window;
   UInt64 m_num_arrivals;
   UInt64 m_service_time_sum; // In ps
   UInt64 m_service_time_sum2; // In ps^2

   void addItem(SubsecondTime pkt_time, SubsecondTime service_time);
   void removeItems(SubsecondTime earliest_time);
};

#endif /* __QUEUE_MODEL_WINDOWED_MG1_H__ */
