#include "queue_model_contention.h"

QueueModelContention::QueueModelContention(String name, UInt32 id, UInt32 num_outstanding)
   : m_contention(name, id, num_outstanding)
{}

QueueModelContention::~QueueModelContention()
{}

SubsecondTime QueueModelContention::computeQueueDelay(SubsecondTime pkt_time, SubsecondTime processing_time, core_id_t requester)
{
   SubsecondTime t_start = pkt_time;
   SubsecondTime t_delay = processing_time;
   SubsecondTime t_complete = m_contention.getCompletionTime(t_start, t_delay);
   SubsecondTime t_queue = t_complete - t_start - t_delay;
   return t_queue;
}
