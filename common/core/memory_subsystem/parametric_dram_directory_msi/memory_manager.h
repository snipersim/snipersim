#pragma once

#include "memory_manager_base.h"
#include "cache_base.h"
#include "cache_cntlr.h"
#include "nuca_cache.h"
#include "../pr_l1_pr_l2_dram_directory_msi/dram_directory_cntlr.h"
#include "../pr_l1_pr_l2_dram_directory_msi/dram_cntlr.h"
#include "address_home_lookup.h"
#include "../pr_l1_pr_l2_dram_directory_msi/shmem_msg.h"
#include "mem_component.h"
#include "sem.h"
#include "fixed_types.h"
#include "shmem_perf_model.h"
#include "shared_cache_block_info.h"
#include "subsecond_time.h"
#include "mmu_base.h"

#include <map>

class DramCache;
class ShmemPerf;

namespace ParametricDramDirectoryMSI
{
   class TLB;

   typedef std::pair<core_id_t, MemComponent::component_t> CoreComponentType;
   typedef std::map<CoreComponentType, CacheCntlr *> CacheCntlrMap;

   class MemoryManagementUnitBase;

   class MemoryManager : public MemoryManagerBase
   {
   private:
      CacheCntlr *m_cache_cntlrs[MemComponent::LAST_LEVEL_CACHE + 1];
      NucaCache *m_nuca_cache;
      DramCache *m_dram_cache;
      PrL1PrL2DramDirectoryMSI::DramDirectoryCntlr *m_dram_directory_cntlr;
      PrL1PrL2DramDirectoryMSI::DramCntlr *m_dram_cntlr;
      AddressHomeLookup *m_tag_directory_home_lookup;
      AddressHomeLookup *m_dram_controller_home_lookup;

		MemoryManagementUnitBase *m_mmu; //	Responsible for handling address translation
		String mmu_type; // MMU type 
		bool m_translation_enabled;

      core_id_t m_core_id_master;

      bool m_tag_directory_present;
      bool m_dram_cntlr_present;

      Semaphore *m_user_thread_sem;
      Semaphore *m_network_thread_sem;

      UInt32 m_cache_block_size;
      MemComponent::component_t m_last_level_cache;
      bool m_enabled;

      ShmemPerf m_dummy_shmem_perf;

      // Performance Models
      CachePerfModel *m_cache_perf_models[MemComponent::LAST_LEVEL_CACHE + 1];

      // Global map of all caches on all cores (within this process!)
      static CacheCntlrMap m_all_cache_cntlrs;

		std::ofstream log_file_mmu;
		std::string log_file_name_mmu;

		struct {
			SubsecondTime m_memory_access_latency;
			SubsecondTime m_translation_latency;
			UInt64 m_memory_accesses;
			UInt64 translation_dram_memory_dram; // Translation required DRAM access and memory access required DRAM access
			UInt64 translation_dram_memory_cache; // Translation required DRAM access and memory access required cache access
			UInt64 translation_cache_memory_dram; // Translation required cache access and memory access required DRAM access
			UInt64 translation_cache_memory_cache; // Translation required cache access and memory access required cache access
			UInt64 translation_slower_than_memory_access;
			UInt64 translation_faster_than_memory_access;
		} memory_access_stats;

   public:
      MemoryManager(Core *core, Network *network, ShmemPerfModel *shmem_perf_model);
      ~MemoryManager();

      UInt64 getCacheBlockSize() const { return m_cache_block_size; }

      Cache *getCache(MemComponent::component_t mem_component)
      {
         return m_cache_cntlrs[mem_component == MemComponent::LAST_LEVEL_CACHE ? MemComponent::component_t(m_last_level_cache) : mem_component]->getCache();
      }
      Cache *getL1ICache() { return getCache(MemComponent::L1_ICACHE); }
      Cache *getL1DCache() { return getCache(MemComponent::L1_DCACHE); }
      Cache *getLastLevelCache() { return getCache(MemComponent::LAST_LEVEL_CACHE); }
      PrL1PrL2DramDirectoryMSI::DramDirectoryCache *getDramDirectoryCache() { return m_dram_directory_cntlr->getDramDirectoryCache(); }
      PrL1PrL2DramDirectoryMSI::DramCntlr *getDramCntlr() { return m_dram_cntlr; }
      AddressHomeLookup *getTagDirectoryHomeLookup() { return m_tag_directory_home_lookup; }
      AddressHomeLookup *getDramControllerHomeLookup() { return m_dram_controller_home_lookup; }

      CacheCntlr *getCacheCntlrAt(core_id_t core_id, MemComponent::component_t mem_component) { return m_all_cache_cntlrs[CoreComponentType(core_id, mem_component)]; }
      void setCacheCntlrAt(core_id_t core_id, MemComponent::component_t mem_component, CacheCntlr *cache_cntlr) { m_all_cache_cntlrs[CoreComponentType(core_id, mem_component)] = cache_cntlr; }

      HitWhere::where_t coreInitiateMemoryAccess(
          IntPtr eip,
          MemComponent::component_t mem_component,
          Core::lock_signal_t lock_signal,
          Core::mem_op_t mem_op_type,
          IntPtr address, UInt32 offset,
          Byte *data_buf, UInt32 data_length,
          Core::MemModeled modeled);

      void handleMsgFromNetwork(NetPacket &packet);

      void sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::msg_t msg_type, MemComponent::component_t sender_mem_component, MemComponent::component_t receiver_mem_component, core_id_t requester, core_id_t receiver, IntPtr address, Byte *data_buf = NULL, UInt32 data_length = 0, HitWhere::where_t where = HitWhere::UNKNOWN, ShmemPerf *perf = NULL, ShmemPerfModel::Thread_t thread_num = ShmemPerfModel::NUM_CORE_THREADS, CacheBlockInfo::block_type_t block_type = CacheBlockInfo::block_type_t::NON_PAGE_TABLE);

      void broadcastMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::msg_t msg_type, MemComponent::component_t sender_mem_component, MemComponent::component_t receiver_mem_component, core_id_t requester, IntPtr address, Byte *data_buf = NULL, UInt32 data_length = 0, ShmemPerf *perf = NULL, ShmemPerfModel::Thread_t thread_num = ShmemPerfModel::NUM_CORE_THREADS);

      SubsecondTime getL1HitLatency(void) { return m_cache_perf_models[MemComponent::L1_ICACHE]->getLatency(CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS); }
      void addL1Hits(bool icache, Core::mem_op_t mem_op_type, UInt64 hits)
      {
         (icache ? m_cache_cntlrs[MemComponent::L1_ICACHE] : m_cache_cntlrs[MemComponent::L1_DCACHE])->updateHits(mem_op_type, hits);
      }

      void enableModels();
      void disableModels();

      core_id_t getShmemRequester(const void *pkt_data)
      {
         return ((PrL1PrL2DramDirectoryMSI::ShmemMsg *)pkt_data)->getRequester();
      }

      UInt32 getModeledLength(const void *pkt_data)
      {
         return ((PrL1PrL2DramDirectoryMSI::ShmemMsg *)pkt_data)->getModeledLength();
      }

      void tagCachesBlockType(IntPtr address, CacheBlockInfo::block_type_t btype)
		{
			if (m_nuca_cache)
				m_nuca_cache->markTranslationMetadata(address, btype);
			this->getCache(MemComponent::component_t::L1_DCACHE)->markMetadata(address, btype);
			this->getCache(MemComponent::component_t::L2_CACHE)->markMetadata(address, btype);
		}

      SubsecondTime getCost(MemComponent::component_t mem_component, CachePerfModel::CacheAccess_t access_type);
      void incrElapsedTime(SubsecondTime latency, ShmemPerfModel::Thread_t thread_num = ShmemPerfModel::NUM_CORE_THREADS);
      void incrElapsedTime(MemComponent::component_t mem_component, CachePerfModel::CacheAccess_t access_type, ShmemPerfModel::Thread_t thread_num = ShmemPerfModel::NUM_CORE_THREADS);
   };
}
