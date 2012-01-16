#include "queue_model_basic.h"
#include "utils.h"
#include "log.h"

QueueModelBasic::QueueModelBasic(String name, UInt32 id,
      bool moving_avg_enabled,
      UInt32 moving_avg_window_size,
      String moving_avg_type_str):
   m_queue_time(SubsecondTime::Zero()),
   m_moving_average(NULL)
{
   if (moving_avg_enabled)
   {
      MovingAverage<SubsecondTime>::AvgType_t moving_avg_type = MovingAverage<SubsecondTime>::parseAvgType(moving_avg_type_str);
      m_moving_average = MovingAverage<SubsecondTime>::createAvgType(moving_avg_type, moving_avg_window_size);
   }
}

QueueModelBasic::~QueueModelBasic()
{}

SubsecondTime
QueueModelBasic::computeQueueDelay(SubsecondTime pkt_time, SubsecondTime processing_time, core_id_t requester)
{
   // Compute the moving average here
   SubsecondTime ref_time;
   if (m_moving_average)
   {
      ref_time = m_moving_average->compute(pkt_time);
   }
   else
   {
      ref_time = pkt_time;
   }

   SubsecondTime queue_delay = (m_queue_time > ref_time) ? (m_queue_time - ref_time) : SubsecondTime::Zero();
   if (queue_delay > 10000 * SubsecondTime::NS())
   {
      LOG_PRINT("Queue Time(%s), Ref Time(%s), Queue Delay(%s), Requester(%i)",
         itostr(m_queue_time).c_str(), itostr(ref_time).c_str(), itostr(queue_delay).c_str(), requester);
   }
   else if ((queue_delay == SubsecondTime::Zero()) && ((ref_time - m_queue_time) > 10000 * SubsecondTime::NS()))
   {
      LOG_PRINT("Queue Time(%s), Ref Time(%s), Difference(%s), Requester(%i)",
            itostr(m_queue_time).c_str(), itostr(ref_time).c_str(), itostr(ref_time - m_queue_time).c_str(), requester);
   }

   // Update the Queue Time
   m_queue_time = getMax<SubsecondTime>(m_queue_time, ref_time) + processing_time;

   return queue_delay;
}
