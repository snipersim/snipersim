#ifndef NVM_PERF_MODEL_H
#define NVM_PERF_MODEL_H

#include "dram_perf_model.h"

class NvmPerfModel : public DramPerfModel
{
public:
   static NvmPerfModel *createNvmPerfModel(core_id_t core_id, UInt32 cache_block_size);
   
   static SubsecondTime getReadLatency();
   static SubsecondTime getWriteLatency();
   static SubsecondTime getLogLatency();

   NvmPerfModel(core_id_t core_id, UInt64 cache_block_size) : DramPerfModel(core_id, cache_block_size) {}
   ~NvmPerfModel() override = default;
};

#endif /* NVM_PERF_MODEL_H */
