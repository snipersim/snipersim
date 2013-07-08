#ifndef __DRAM_CNTLR_INTERFACE_H
#define __DRAM_CNTLR_INTERFACE_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include "hit_where.h"
#include "shmem_msg.h"

#include "boost/tuple/tuple.hpp"

class MemoryManagerBase;
class ShmemPerfModel;
class ShmemPerf;

class DramCntlrInterface
{
   protected:
      MemoryManagerBase* m_memory_manager;
      ShmemPerfModel* m_shmem_perf_model;
      UInt32 m_cache_block_size;

      UInt32 getCacheBlockSize() { return m_cache_block_size; }
      MemoryManagerBase* getMemoryManager() { return m_memory_manager; }
      ShmemPerfModel* getShmemPerfModel() { return m_shmem_perf_model; }

   public:
      typedef enum
      {
         READ = 0,
         WRITE,
         NUM_ACCESS_TYPES
      } access_t;

      DramCntlrInterface(MemoryManagerBase* memory_manager, ShmemPerfModel* shmem_perf_model, UInt32 cache_block_size)
         : m_memory_manager(memory_manager)
         , m_shmem_perf_model(shmem_perf_model)
         , m_cache_block_size(cache_block_size)
      {}
      virtual ~DramCntlrInterface() {}

      virtual boost::tuple<SubsecondTime, HitWhere::where_t> getDataFromDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now, ShmemPerf *perf) = 0;
      virtual boost::tuple<SubsecondTime, HitWhere::where_t> putDataToDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now) = 0;

      void handleMsgFromTagDirectory(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg);
};

#endif // __DRAM_CNTLR_INTERFACE_H
