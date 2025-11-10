#pragma once

#include "distribution.h"
#include "dram_cntlr_interface.h"
#include "nvm_perf_model.h"
#include "queue_model.h"
#include "subsecond_time.h"

class NvmPerfModelNormal : public NvmPerfModel
{
public:
   NvmPerfModelNormal(core_id_t core_id, UInt32 cache_block_size);
   ~NvmPerfModelNormal() override;

private:
   QueueModel* m_queue_model;
   TimeDistribution* m_nvm_read_cost;
   TimeDistribution* m_nvm_write_cost;
   SubsecondTime m_total_queueing_delay;

   SubsecondTime computeQueueDelay(SubsecondTime pkt_time, SubsecondTime processing_time, core_id_t requester,
                                   DramCntlrInterface::access_t access_type) override;
   void increaseQueueDelay(DramCntlrInterface::access_t access_type, SubsecondTime queue_delay) override;

   SubsecondTime getReadCost() override { return m_nvm_read_cost->next(); }
   SubsecondTime getWriteCost() override { return m_nvm_write_cost->next(); }
};
