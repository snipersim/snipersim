#ifndef __QUEUE_MODEL_HISTORY_LIST_H__
#define __QUEUE_MODEL_HISTORY_LIST_H__

#include <list>

#include "queue_model.h"
#include "fixed_types.h"
#include "moving_average.h"

class QueueModelHistoryList : public QueueModel
{
public:
   typedef std::list<std::pair<SubsecondTime,SubsecondTime> > FreeIntervalList;

   QueueModelHistoryList(String name, UInt32 id, SubsecondTime min_processing_time);
   ~QueueModelHistoryList();

   SubsecondTime computeQueueDelay(SubsecondTime pkt_time, SubsecondTime processing_time, core_id_t requester = INVALID_CORE_ID);

   float getQueueUtilization();
   float getFracRequestsUsingAnalyticalModel();

private:
   SubsecondTime m_min_processing_time;
   UInt32 m_max_free_interval_list_size;

   FreeIntervalList m_free_interval_list;

   // Tracks queue utilization
   SubsecondTime m_utilized_time;
   SubsecondTime m_total_queue_delay;
   MovingAverage<SubsecondTime>* m_average_delay;

   // Is analytical model used ?
   bool m_analytical_model_enabled;

   // Performance Counters
   UInt64 m_total_requests;
   UInt64 m_total_requests_using_analytical_model;

   void updateQueueUtilization(SubsecondTime processing_time);
   void updateAverageDelay(SubsecondTime queue_delay);
   SubsecondTime computeUsingHistoryList(SubsecondTime pkt_time, SubsecondTime processing_time);
   SubsecondTime computeUsingAnalyticalModel(SubsecondTime pkt_time, SubsecondTime processing_time);
};

#endif /* __QUEUE_MODEL_HISTORY_LIST_H__ */
