#ifndef NVM_PERF_MODEL_READWRITE_H
#define NVM_PERF_MODEL_READWRITE_H

#include "nvm_perf_model.h"
#include "queue_model.h"
#include "fixed_types.h"
#include "subsecond_time.h"
#include "dram_cntlr_interface.h"

class NvmPerfModelReadWrite : public NvmPerfModel
{
private:
   QueueModel *m_queue_model_read;
   QueueModel *m_queue_model_write;
   SubsecondTime m_nvm_read_cost;
   SubsecondTime m_nvm_write_cost;
   SubsecondTime m_nvm_log_cost;
   ComponentBandwidth m_nvm_bandwidth;
   bool m_shared_readwrite;

   SubsecondTime m_total_read_queueing_delay;
   SubsecondTime m_total_write_queueing_delay;
   SubsecondTime m_total_access_latency;

public:
   NvmPerfModelReadWrite(core_id_t core_id, UInt32 cache_block_size);

   ~NvmPerfModelReadWrite();

   SubsecondTime getAccessLatency(SubsecondTime pkt_time, UInt64 pkt_size, core_id_t requester, IntPtr address,
                                  DramCntlrInterface::access_t access_type, ShmemPerf *perf);
};

#endif // NVM_PERF_MODEL_READWRITE_H
