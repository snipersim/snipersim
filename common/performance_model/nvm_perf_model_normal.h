#ifndef NVM_PERF_MODEL_NORMAL_H
#define NVM_PERF_MODEL_NORMAL_H

#include "nvm_perf_model.h"
#include "queue_model.h"
#include "fixed_types.h"
#include "subsecond_time.h"
#include "dram_cntlr_interface.h"
#include "distribution.h"

class NvmPerfModelNormal : public NvmPerfModel
{
private:
   QueueModel* m_queue_model;
   TimeDistribution* m_nvm_read_cost;
   TimeDistribution* m_nvm_write_cost;
   TimeDistribution* m_nvm_log_cost;
   ComponentBandwidth m_nvm_bandwidth;

   SubsecondTime m_total_queueing_delay;
   SubsecondTime m_total_access_latency;

public:
   NvmPerfModelNormal(core_id_t core_id, UInt32 cache_block_size);

   ~NvmPerfModelNormal();

   SubsecondTime getAccessLatency(SubsecondTime pkt_time, UInt64 pkt_size, core_id_t requester, IntPtr address,
                                  DramCntlrInterface::access_t access_type, ShmemPerf *perf);
};

#endif // NVM_PERF_MODEL_NORMAL_H
