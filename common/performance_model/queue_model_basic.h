#ifndef __QUEUE_MODEL_BASIC_H__
#define __QUEUE_MODEL_BASIC_H__

#include "queue_model.h"
#include "moving_average.h"
#include "fixed_types.h"
#include "subsecond_time.h"

class QueueModelBasic : public QueueModel
{
public:
   QueueModelBasic(String name, UInt32 id, bool moving_avg_enabled, UInt32 moving_avg_window_size, String moving_avg_type_str);
   ~QueueModelBasic();

   SubsecondTime computeQueueDelay(SubsecondTime pkt_time, SubsecondTime processing_time, core_id_t requester = INVALID_CORE_ID);

private:
   SubsecondTime m_queue_time;
   MovingAverage<SubsecondTime>* m_moving_average;
};

#endif /* __QUEUE_MODEL_BASIC_H__ */
