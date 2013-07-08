#ifndef __DRAM_PERF_MODEL_NORMAL_H__
#define __DRAM_PERF_MODEL_NORMAL_H__

#include "dram_perf_model.h"
#include "queue_model.h"
#include "fixed_types.h"
#include "subsecond_time.h"
#include "dram_cntlr_interface.h"
#include "distribution.h"

class DramPerfModelNormal : public DramPerfModel
{
   private:
      QueueModel* m_queue_model;
      TimeDistribution* m_dram_access_cost;
      ComponentBandwidth m_dram_bandwidth;

      SubsecondTime m_total_queueing_delay;
      SubsecondTime m_total_access_latency;

   public:
      DramPerfModelNormal(core_id_t core_id,
            UInt32 cache_block_size);

      ~DramPerfModelNormal();

      SubsecondTime getAccessLatency(SubsecondTime pkt_time, UInt64 pkt_size, core_id_t requester, IntPtr address, DramCntlrInterface::access_t access_type, ShmemPerf *perf);
};

#endif /* __DRAM_PERF_MODEL_NORMAL_H__ */
