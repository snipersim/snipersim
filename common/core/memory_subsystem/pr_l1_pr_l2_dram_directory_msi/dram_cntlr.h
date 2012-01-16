#pragma once

// Define to re-enable DramAccessCount
//#define ENABLE_DRAM_ACCESS_COUNT

#include <unordered_map>

#include "dram_perf_model.h"
#include "shmem_perf_model.h"
#include "shmem_msg.h"
#include "fixed_types.h"
#include "memory_manager_base.h"
#include "subsecond_time.h"

namespace PrL1PrL2DramDirectoryMSI
{
   class DramCntlr
   {
      public:
         typedef enum
         {
            READ = 0,
            WRITE,
            NUM_ACCESS_TYPES
         } access_t;

      private:
         MemoryManagerBase* m_memory_manager;
         std::unordered_map<IntPtr, Byte*> m_data_map;
         DramPerfModel* m_dram_perf_model;
         UInt32 m_cache_block_size;
         ShmemPerfModel* m_shmem_perf_model;

         typedef std::unordered_map<IntPtr,UInt64> AccessCountMap;
         AccessCountMap* m_dram_access_count;
         UInt64 m_reads, m_writes;

         UInt32 getCacheBlockSize() { return m_cache_block_size; }
         MemoryManagerBase* getMemoryManager() { return m_memory_manager; }
         ShmemPerfModel* getShmemPerfModel() { return m_shmem_perf_model; }
         SubsecondTime runDramPerfModel(core_id_t requester);

         void addToDramAccessCount(IntPtr address, access_t access_type);
         void printDramAccessCount(void);

      public:
         DramCntlr(MemoryManagerBase* memory_manager,
               SubsecondTime dram_access_cost,
               ComponentBandwidth dram_bandwidth,
               bool dram_queue_model_enabled,
               String dram_queue_model_type,
               UInt32 cache_block_size,
               ShmemPerfModel* shmem_perf_model);

         ~DramCntlr();

         DramPerfModel* getDramPerfModel() { return m_dram_perf_model; }

         void getDataFromDram(IntPtr address, core_id_t requester, Byte* data_buf);
         void putDataToDram(IntPtr address, core_id_t requester, Byte* data_buf);
   };
}
