#ifndef __QUEUE_MODEL_CONTENTION_H__
#define __QUEUE_MODEL_CONTENTION_H__

#include "queue_model.h"
#include "fixed_types.h"
#include "contention_model.h"

class QueueModelContention : public QueueModel
{
public:
   QueueModelContention(String name, UInt32 id, UInt32 num_outstanding = 1);
   ~QueueModelContention();

   SubsecondTime computeQueueDelay(SubsecondTime pkt_time, SubsecondTime processing_time, core_id_t requester = INVALID_CORE_ID);

private:
   ContentionModel m_contention;
};

#endif /* __QUEUE_MODEL_CONTENTION_H__ */

