#pragma once

#include "dram_perf_model.h"

class NvmPerfModel : public DramPerfModel
{
public:
   NvmPerfModel(core_id_t core_id, UInt64 cache_block_size);
   ~NvmPerfModel() override;

   SubsecondTime getAccessLatency(SubsecondTime pkt_time, UInt64 pkt_size, core_id_t requester, IntPtr address,
                                  DramCntlrInterface::access_t access_type, ShmemPerf* perf) override;

   static NvmPerfModel* createNvmPerfModel(core_id_t core_id, UInt32 cache_block_size);

protected:
   ComponentBandwidth m_nvm_bandwidth;
   SubsecondTime m_total_access_latency;

   SubsecondTime computeAccessCost(DramCntlrInterface::access_t access_type);

   virtual SubsecondTime computeQueueDelay(SubsecondTime pkt_time, SubsecondTime processing_time, core_id_t requester,
                                           DramCntlrInterface::access_t access_type)                    = 0;
   virtual void increaseQueueDelay(DramCntlrInterface::access_t access_type, SubsecondTime queue_delay) = 0;

   virtual SubsecondTime getReadCost()  = 0;
   virtual SubsecondTime getWriteCost() = 0;

   static SubsecondTime getLatency(const String &param);
   static SubsecondTime getReadLatency() { return getLatency("read_latency"); }
   static SubsecondTime getWriteLatency() { return getLatency("write_latency"); }

   static DramPerfModel* createDramPerfModel(core_id_t core_id, UInt32 cache_block_size) {
      return createNvmPerfModel(core_id, cache_block_size);
   }
};
