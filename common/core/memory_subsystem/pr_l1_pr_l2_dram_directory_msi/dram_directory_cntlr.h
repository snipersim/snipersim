#pragma once

#include "dram_directory_cache.h"
#include "req_queue_list.h"
#include "dram_cntlr.h"
#include "address_home_lookup.h"
#include "shmem_req.h"
#include "shmem_msg.h"
#include "shmem_perf.h"
#include "mem_component.h"
#include "memory_manager_base.h"
#include "coherency_protocol.h"

class NucaCache;

namespace PrL1PrL2DramDirectoryMSI
{
   class DramDirectoryCntlr
   {
      private:
         // Functional Models
         MemoryManagerBase* m_memory_manager;
         AddressHomeLookup* m_dram_controller_home_lookup;
         DramDirectoryCache* m_dram_directory_cache;
         ReqQueueList* m_dram_directory_req_queue_list;

         NucaCache* m_nuca_cache;

         core_id_t m_core_id;
         UInt32 m_cache_block_size;

         ShmemPerfModel* m_shmem_perf_model;

         CoherencyProtocol::type_t m_protocol;

         UInt64 evict[DirectoryState::NUM_DIRECTORY_STATES];
         UInt64 forward, forward_failed;

         UInt32 getCacheBlockSize() { return m_cache_block_size; }
         MemoryManagerBase* getMemoryManager() { return m_memory_manager; }
         ShmemPerfModel* getShmemPerfModel() { return m_shmem_perf_model; }

         // Private Functions
         DirectoryEntry* processDirectoryEntryAllocationReq(ShmemReq* shmem_req);
         void processNullifyReq(ShmemReq* shmem_req);

         void processNextReqFromL2Cache(IntPtr address);
         void processExReqFromL2Cache(ShmemReq* shmem_req, Byte* cached_data_buf = NULL);
         void processShReqFromL2Cache(ShmemReq* shmem_req, Byte* cached_data_buf = NULL);
         void retrieveDataAndSendToL2Cache(ShmemMsg::msg_t reply_msg_type, core_id_t receiver, IntPtr address, Byte* cached_data_buf, ShmemMsg *orig_shmem_msg);
         void processDRAMReply(core_id_t sender, ShmemMsg* shmem_msg);

         void processUpgradeReqFromL2Cache(ShmemReq* shmem_req, Byte* cached_data_buf = NULL);

         void processInvRepFromL2Cache(core_id_t sender, ShmemMsg* shmem_msg);
         void processFlushRepFromL2Cache(core_id_t sender, ShmemMsg* shmem_msg);
         void processWbRepFromL2Cache(core_id_t sender, ShmemMsg* shmem_msg);
         void sendDataToNUCA(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now);
         void sendDataToDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now);

         void updateShmemPerf(ShmemReq *shmem_req, ShmemPerf::shmem_times_type_t reason = ShmemPerf::UNKNOWN)
         {
            updateShmemPerf(shmem_req->getShmemMsg(), reason);
         }
         void updateShmemPerf(ShmemMsg *shmem_msg, ShmemPerf::shmem_times_type_t reason = ShmemPerf::UNKNOWN)
         {
            shmem_msg->getPerf()->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD), reason);
         }

      public:
         DramDirectoryCntlr(core_id_t core_id,
               MemoryManagerBase* memory_manager,
               AddressHomeLookup* dram_controller_home_lookup,
               NucaCache* nuca_cache,
               UInt32 dram_directory_total_entries,
               UInt32 dram_directory_associativity,
               UInt32 cache_block_size,
               UInt32 dram_directory_max_num_sharers,
               UInt32 dram_directory_max_hw_sharers,
               String dram_directory_type_str,
               ComponentLatency dram_directory_cache_access_time,
               ShmemPerfModel* shmem_perf_model);
         ~DramDirectoryCntlr();

         void handleMsgFromL2Cache(core_id_t sender, ShmemMsg* shmem_msg);
         void handleMsgFromDRAM(core_id_t sender, ShmemMsg* shmem_msg);

         DramDirectoryCache* getDramDirectoryCache() { return m_dram_directory_cache; }
   };

}
