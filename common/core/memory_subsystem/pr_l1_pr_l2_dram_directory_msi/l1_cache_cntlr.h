#pragma once

// Forward declaration
namespace PrL1PrL2DramDirectoryMSI
{
   class L2CacheCntlr;
   class MemoryManager;
}

#include "core.h"
#include "cache.h"
#include "pr_l1_cache_block_info.h"
#include "shmem_msg.h"
#include "mem_component.h"
#include "semaphore.h"
#include "lock.h"
#include "fixed_types.h"
#include "shmem_perf_model.h"

namespace PrL1PrL2DramDirectoryMSI
{
   class L1CacheCntlr
   {
      private:
         MemoryManager* m_memory_manager;
         Cache* m_l1_icache;
         Cache* m_l1_dcache;
         L2CacheCntlr* m_l2_cache_cntlr;

         core_id_t m_core_id;
         UInt32 m_cache_block_size;

         Lock m_l1_icache_lock;
         Lock m_l1_dcache_lock;
         Semaphore* m_user_thread_sem;
         Semaphore* m_network_thread_sem;

         ShmemPerfModel* m_shmem_perf_model;

         // Private Functions
         void accessCache(MemComponent::component_t mem_component,
               Core::mem_op_t mem_op_type, 
               IntPtr ca_address, UInt32 offset,
               Byte* data_buf, UInt32 data_length);
         bool operationPermissibleinL1Cache(
               MemComponent::component_t mem_component, 
               IntPtr address, Core::mem_op_t mem_op_type,
               UInt32 access_num, bool modeled);

         Cache* getL1Cache(MemComponent::component_t mem_component);
         ShmemMsg::msg_t getShmemMsgType(Core::mem_op_t mem_op_type);

         // Get Cache Block Size
         UInt32 getCacheBlockSize(void) { return m_cache_block_size; }
         MemoryManager* getMemoryManager() { return m_memory_manager; }
         ShmemPerfModel* getShmemPerfModel() { return m_shmem_perf_model; }

         // Wait for Network Thread
         void waitForNetworkThread(void);
         // Wake up Network Thread
         void wakeUpNetworkThread(void);
         
      public:
         
         L1CacheCntlr(core_id_t core_id,
               MemoryManager* memory_manager,
               Semaphore* user_thread_sem,
               Semaphore* network_thread_sem,
               UInt32 cache_block_size,
               UInt32 l1_icache_size, UInt32 l1_icache_associativity,
               String l1_icache_replacement_policy,
               UInt32 l1_dcache_size, UInt32 l1_dcache_associativity,
               String l1_dcache_replacement_policy,
               ShmemPerfModel* shmem_perf_model);
         
         ~L1CacheCntlr();

         Cache* getL1ICache() { return m_l1_icache; }
         Cache* getL1DCache() { return m_l1_dcache; }

         void setL2CacheCntlr(L2CacheCntlr* l2_cache_cntlr);

         bool processMemOpFromCore(
               MemComponent::component_t mem_component,
               Core::lock_signal_t lock_signal,
               Core::mem_op_t mem_op_type, 
               IntPtr ca_address, UInt32 offset,
               Byte* data_buf, UInt32 data_length,
               bool modeled);

         void insertCacheBlock(MemComponent::component_t mem_component,
               IntPtr address, CacheState::cstate_t cstate, Byte* data_buf,
               bool* eviction_ptr, IntPtr* evict_address_ptr);

         CacheState::cstate_t getCacheState(
               MemComponent::component_t mem_component, IntPtr address);
         void setCacheState(MemComponent::component_t mem_component,
               IntPtr address, CacheState::cstate_t cstate);
         void invalidateCacheBlock(
               MemComponent::component_t mem_component, IntPtr address);

         void acquireLock(MemComponent::component_t mem_component);
         void releaseLock(MemComponent::component_t mem_component);
   };
}
