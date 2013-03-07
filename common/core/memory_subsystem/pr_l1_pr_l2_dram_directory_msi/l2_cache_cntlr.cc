#include "l1_cache_cntlr.h"
#include "l2_cache_cntlr.h"
#include "log.h"
#include "memory_manager.h"

namespace PrL1PrL2DramDirectoryMSI
{

L2CacheCntlr::L2CacheCntlr(core_id_t core_id,
      MemoryManager* memory_manager,
      L1CacheCntlr* l1_cache_cntlr,
      AddressHomeLookup* dram_directory_home_lookup,
      Semaphore* user_thread_sem,
      Semaphore* network_thread_sem,
      UInt32 cache_block_size,
      UInt32 l2_cache_size, UInt32 l2_cache_associativity,
      String l2_cache_replacement_policy,
      ShmemPerfModel* shmem_perf_model):
   m_memory_manager(memory_manager),
   m_l1_cache_cntlr(l1_cache_cntlr),
   m_dram_directory_home_lookup(dram_directory_home_lookup),
   m_core_id(core_id),
   m_cache_block_size(cache_block_size),
   m_user_thread_sem(user_thread_sem),
   m_network_thread_sem(network_thread_sem),
   m_shmem_perf_model(shmem_perf_model)
{
   m_l2_cache = new Cache("L2",
         "perf_model/l2_cache",
         core_id,
         l2_cache_size,
         l2_cache_associativity,
         m_cache_block_size,
         l2_cache_replacement_policy,
         CacheBase::PR_L2_CACHE);
}

L2CacheCntlr::~L2CacheCntlr()
{
   delete m_l2_cache;
}

PrL2CacheBlockInfo*
L2CacheCntlr::getCacheBlockInfo(IntPtr address)
{
   return (PrL2CacheBlockInfo*) m_l2_cache->peekSingleLine(address);
}

CacheState::cstate_t
L2CacheCntlr::getCacheState(PrL2CacheBlockInfo* l2_cache_block_info)
{
   return (l2_cache_block_info == NULL) ? CacheState::INVALID : l2_cache_block_info->getCState();
}

void
L2CacheCntlr::setCacheState(PrL2CacheBlockInfo* l2_cache_block_info, CacheState::cstate_t cstate)
{
   assert(l2_cache_block_info);
   l2_cache_block_info->setCState(cstate);
}

void
L2CacheCntlr::invalidateCacheBlock(IntPtr address)
{
   m_l2_cache->invalidateSingleLine(address);
}

void
L2CacheCntlr::retrieveCacheBlock(IntPtr address, Byte* data_buf)
{
   __attribute(__unused__) PrL2CacheBlockInfo* l2_cache_block_info = (PrL2CacheBlockInfo*) m_l2_cache->accessSingleLine(address, Cache::LOAD, data_buf, getCacheBlockSize(), getShmemPerfModel()->getElapsedTime());
   assert(l2_cache_block_info);
}

void
L2CacheCntlr::writeCacheBlock(IntPtr address, UInt32 offset, Byte* data_buf, UInt32 data_length)
{
   __attribute(__unused__) PrL2CacheBlockInfo* l2_cache_block_info = (PrL2CacheBlockInfo*) m_l2_cache->accessSingleLine(address + offset, Cache::STORE, data_buf, data_length, getShmemPerfModel()->getElapsedTime());
   assert(l2_cache_block_info);
}

PrL2CacheBlockInfo*
L2CacheCntlr::insertCacheBlock(IntPtr address, CacheState::cstate_t cstate, Byte* data_buf)
{
   bool eviction;
   IntPtr evict_address;
   PrL2CacheBlockInfo evict_block_info;
   Byte evict_buf[getCacheBlockSize()];

   m_l2_cache->insertSingleLine(address, data_buf,
         &eviction, &evict_address, &evict_block_info, evict_buf,
         getShmemPerfModel()->getElapsedTime());
   PrL2CacheBlockInfo* l2_cache_block_info = getCacheBlockInfo(address);
   setCacheState(l2_cache_block_info, cstate);

   if (eviction)
   {
      LOG_PRINT("Eviction: addr(0x%x)", evict_address);
      invalidateCacheBlockInL1(evict_block_info.getCachedLoc(), evict_address);

      UInt32 home_node_id = getHome(evict_address);
      if (evict_block_info.getCState() == CacheState::MODIFIED)
      {
         // Send back the data also
         getMemoryManager()->sendMsg(ShmemMsg::FLUSH_REP,
               MemComponent::L2_CACHE, MemComponent::TAG_DIR,
               m_core_id /* requester */,
               home_node_id /* receiver */,
               evict_address,
               evict_buf, getCacheBlockSize());
      }
      else
      {
         LOG_ASSERT_ERROR(evict_block_info.getCState() == CacheState::SHARED,
               "evict_address(0x%x), evict_state(%u), cached_loc(%u)",
               evict_address, evict_block_info.getCState(), evict_block_info.getCachedLoc());
         getMemoryManager()->sendMsg(ShmemMsg::INV_REP,
               MemComponent::L2_CACHE, MemComponent::TAG_DIR,
               m_core_id /* requester */,
               home_node_id /* receiver */,
               evict_address);
      }
   }

   return l2_cache_block_info;
}

void
L2CacheCntlr::setCacheStateInL1(MemComponent::component_t mem_component, IntPtr address, CacheState::cstate_t cstate)
{
   if (mem_component != MemComponent::INVALID_MEM_COMPONENT)
      m_l1_cache_cntlr->setCacheState(mem_component, address, cstate);
}

void
L2CacheCntlr::invalidateCacheBlockInL1(MemComponent::component_t mem_component, IntPtr address)
{
   if (mem_component != MemComponent::INVALID_MEM_COMPONENT)
      m_l1_cache_cntlr->invalidateCacheBlock(mem_component, address);
}

void
L2CacheCntlr::insertCacheBlockInL1(MemComponent::component_t mem_component,
      IntPtr address, PrL2CacheBlockInfo* l2_cache_block_info,
      CacheState::cstate_t cstate, Byte* data_buf)
{
   bool eviction;
   IntPtr evict_address;

   // Insert the Cache Block in L1 Cache
   m_l1_cache_cntlr->insertCacheBlock(mem_component, address, cstate, data_buf, &eviction, &evict_address);

   // Set the Present bit in L2 Cache corresponding to the inserted block
   l2_cache_block_info->setCachedLoc(mem_component);

   if (eviction)
   {
      // Clear the Present bit in L2 Cache corresponding to the evicted block
      PrL2CacheBlockInfo* evict_block_info = getCacheBlockInfo(evict_address);
      evict_block_info->clearCachedLoc(mem_component);
   }
}

bool
L2CacheCntlr::processShmemReqFromL1Cache(MemComponent::component_t req_mem_component, ShmemMsg::msg_t msg_type, IntPtr address, bool modeled)
{
   PrL2CacheBlockInfo* l2_cache_block_info = getCacheBlockInfo(address);
   CacheState::cstate_t cstate = getCacheState(l2_cache_block_info);

   bool shmem_req_ends_in_l2_cache = shmemReqEndsInL2Cache(msg_type, cstate, modeled);
   if (shmem_req_ends_in_l2_cache)
   {
      Byte data_buf[getCacheBlockSize()];
      retrieveCacheBlock(address, data_buf);

      insertCacheBlockInL1(req_mem_component, address, l2_cache_block_info, cstate, data_buf);
   }

   return shmem_req_ends_in_l2_cache;
}

void
L2CacheCntlr::handleMsgFromL1Cache(ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
   ShmemMsg::msg_t shmem_msg_type = shmem_msg->getMsgType();
   MemComponent::component_t sender_mem_component = shmem_msg->getSenderMemComponent();

   acquireLock();

   assert(shmem_msg->getDataBuf() == NULL);
   assert(shmem_msg->getDataLength() == 0);

   m_shmem_req_source_map[address] = sender_mem_component;
   switch (shmem_msg_type)
   {
      case ShmemMsg::EX_REQ:
         processExReqFromL1Cache(shmem_msg);
         break;

      case ShmemMsg::SH_REQ:
         processShReqFromL1Cache(shmem_msg);
         break;

      default:
         LOG_PRINT_ERROR("Unrecognized shmem msg type (%u)", shmem_msg_type);
         break;
   }

   releaseLock();
}

void
L2CacheCntlr::processExReqFromL1Cache(ShmemMsg* shmem_msg)
{
   // We need to send a request to the Dram Directory Cache
   IntPtr address = shmem_msg->getAddress();

   PrL2CacheBlockInfo* l2_cache_block_info = getCacheBlockInfo(address);
   CacheState::cstate_t cstate = getCacheState(l2_cache_block_info);

   assert((cstate == CacheState::INVALID) || (cstate == CacheState::SHARED));
   if (cstate == CacheState::SHARED)
   {
      // This will clear the 'Present' bit also
      invalidateCacheBlock(address);
      getMemoryManager()->sendMsg(ShmemMsg::INV_REP,
            MemComponent::L2_CACHE, MemComponent::TAG_DIR,
            m_core_id /* requester */,
            getHome(address) /* receiver */,
            address);
   }

   getMemoryManager()->sendMsg(ShmemMsg::EX_REQ,
         MemComponent::L2_CACHE, MemComponent::TAG_DIR,
         m_core_id /* requester */,
         getHome(address) /* receiver */,
         address);
}

void
L2CacheCntlr::processShReqFromL1Cache(ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();

   getMemoryManager()->sendMsg(ShmemMsg::SH_REQ,
         MemComponent::L2_CACHE, MemComponent::TAG_DIR,
         m_core_id /* requester */,
         getHome(address) /* receiver */,
         address);
}

void
L2CacheCntlr::handleMsgFromDramDirectory(
      core_id_t sender, ShmemMsg* shmem_msg)
{
   ShmemMsg::msg_t shmem_msg_type = shmem_msg->getMsgType();
   IntPtr address = shmem_msg->getAddress();

   // Acquire Locks
   MemComponent::component_t caching_mem_component = acquireL1CacheLock(shmem_msg_type, address);
   acquireLock();

   switch (shmem_msg_type)
   {
      case ShmemMsg::EX_REP:
         processExRepFromDramDirectory(sender, shmem_msg);
         break;
      case ShmemMsg::SH_REP:
         processShRepFromDramDirectory(sender, shmem_msg);
         break;
      case ShmemMsg::INV_REQ:
         processInvReqFromDramDirectory(sender, shmem_msg);
         break;
      case ShmemMsg::FLUSH_REQ:
         processFlushReqFromDramDirectory(sender, shmem_msg);
         break;
      case ShmemMsg::WB_REQ:
         processWbReqFromDramDirectory(sender, shmem_msg);
         break;
      default:
         LOG_PRINT_ERROR("Unrecognized msg type: %u", shmem_msg_type);
         break;
   }

   // Release Locks
   releaseLock();
   if (caching_mem_component != MemComponent::INVALID_MEM_COMPONENT)
      m_l1_cache_cntlr->releaseLock(caching_mem_component);

   if ((shmem_msg_type == ShmemMsg::EX_REP) || (shmem_msg_type == ShmemMsg::SH_REP))
   {
      wakeUpUserThread();
      waitForUserThread();
   }
}

void
L2CacheCntlr::processExRepFromDramDirectory(core_id_t sender, ShmemMsg* shmem_msg)
{
   // Update Shared Mem perf counters for access to L2 Cache
   getMemoryManager()->incrElapsedTime(MemComponent::L2_CACHE, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS);

   IntPtr address = shmem_msg->getAddress();
   Byte* data_buf = shmem_msg->getDataBuf();

   PrL2CacheBlockInfo* l2_cache_block_info = insertCacheBlock(address, CacheState::MODIFIED, data_buf);

   // Insert Cache Block in L1 Cache
   // Support for non-blocking caches can be added in this way
   MemComponent::component_t mem_component = m_shmem_req_source_map[address];
   assert (mem_component == MemComponent::L1_DCACHE);
   insertCacheBlockInL1(mem_component, address, l2_cache_block_info, CacheState::MODIFIED, data_buf);

   // Set the Counters in the Shmem Perf model accordingly
   // Set the counter value in the USER thread to that in the SIM thread
   getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD,
         getShmemPerfModel()->getElapsedTime());
}

void
L2CacheCntlr::processShRepFromDramDirectory(core_id_t sender, ShmemMsg* shmem_msg)
{
   // Update Shared Mem perf counters for access to L2 Cache
   getMemoryManager()->incrElapsedTime(MemComponent::L2_CACHE, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS);

   IntPtr address = shmem_msg->getAddress();
   Byte* data_buf = shmem_msg->getDataBuf();

   // Insert Cache Block in L2 Cache
   PrL2CacheBlockInfo* l2_cache_block_info = insertCacheBlock(address, CacheState::SHARED, data_buf);

   // Insert Cache Block in L1 Cache
   // Support for non-blocking caches can be added in this way
   MemComponent::component_t mem_component = m_shmem_req_source_map[address];
   insertCacheBlockInL1(mem_component, address, l2_cache_block_info, CacheState::SHARED, data_buf);

   // Set the Counters in the Shmem Perf model accordingly
   // Set the counter value in the USER thread to that in the SIM thread
   getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD,
         getShmemPerfModel()->getElapsedTime());
}

void
L2CacheCntlr::processInvReqFromDramDirectory(core_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();

   PrL2CacheBlockInfo* l2_cache_block_info = getCacheBlockInfo(address);
   CacheState::cstate_t cstate = getCacheState(l2_cache_block_info);
   if (cstate != CacheState::INVALID)
   {
      assert(cstate == CacheState::SHARED);

      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(MemComponent::L2_CACHE, CachePerfModel::ACCESS_CACHE_TAGS);
      // Update Shared Mem perf counters for access to L1 Cache
      getMemoryManager()->incrElapsedTime(l2_cache_block_info->getCachedLoc(), CachePerfModel::ACCESS_CACHE_TAGS);

      // Invalidate the line in L1 Cache
      invalidateCacheBlockInL1(l2_cache_block_info->getCachedLoc(), address);
      // Invalidate the line in the L2 cache also
      invalidateCacheBlock(address);

      getMemoryManager()->sendMsg(ShmemMsg::INV_REP,
            MemComponent::L2_CACHE, MemComponent::TAG_DIR,
            shmem_msg->getRequester() /* requester */,
            sender /* receiver */,
            address);
   }
   else
   {
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(MemComponent::L2_CACHE, CachePerfModel::ACCESS_CACHE_TAGS);
   }
}

void
L2CacheCntlr::processFlushReqFromDramDirectory(core_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();

   PrL2CacheBlockInfo* l2_cache_block_info = getCacheBlockInfo(address);
   CacheState::cstate_t cstate = getCacheState(l2_cache_block_info);
   if (cstate != CacheState::INVALID)
   {
      assert(cstate == CacheState::MODIFIED);

      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(MemComponent::L2_CACHE, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS);
      // Update Shared Mem perf counters for access to L1 Cache
      getMemoryManager()->incrElapsedTime(l2_cache_block_info->getCachedLoc(), CachePerfModel::ACCESS_CACHE_TAGS);

      // Invalidate the line in L1 Cache
      invalidateCacheBlockInL1(l2_cache_block_info->getCachedLoc(), address);

      // Flush the line
      Byte data_buf[getCacheBlockSize()];
      retrieveCacheBlock(address, data_buf);
      invalidateCacheBlock(address);

      getMemoryManager()->sendMsg(ShmemMsg::FLUSH_REP,
            MemComponent::L2_CACHE, MemComponent::TAG_DIR,
            shmem_msg->getRequester() /* requester */,
            sender /* receiver */,
            address,
            data_buf, getCacheBlockSize());
   }
   else
   {
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(MemComponent::L2_CACHE, CachePerfModel::ACCESS_CACHE_TAGS);
   }
}

void
L2CacheCntlr::processWbReqFromDramDirectory(core_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();

   PrL2CacheBlockInfo* l2_cache_block_info = getCacheBlockInfo(address);
   CacheState::cstate_t cstate = getCacheState(l2_cache_block_info);
   if (cstate != CacheState::INVALID)
   {
      assert(cstate == CacheState::MODIFIED);

      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(MemComponent::L2_CACHE, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS);
      // Update Shared Mem perf counters for access to L1 Cache
      getMemoryManager()->incrElapsedTime(l2_cache_block_info->getCachedLoc(), CachePerfModel::ACCESS_CACHE_TAGS);

      // Set the Appropriate Cache State in L1 also
      setCacheStateInL1(l2_cache_block_info->getCachedLoc(), address, CacheState::SHARED);

      // Write-Back the line
      Byte data_buf[getCacheBlockSize()];
      retrieveCacheBlock(address, data_buf);
      setCacheState(l2_cache_block_info, CacheState::SHARED);

      getMemoryManager()->sendMsg(ShmemMsg::WB_REP,
            MemComponent::L2_CACHE, MemComponent::TAG_DIR,
            shmem_msg->getRequester() /* requester */,
            sender /* receiver */,
            address,
            data_buf, getCacheBlockSize());
   }
   else
   {
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(MemComponent::L2_CACHE, CachePerfModel::ACCESS_CACHE_TAGS);
   }
}

bool
L2CacheCntlr::shmemReqEndsInL2Cache(ShmemMsg::msg_t shmem_msg_type, CacheState::cstate_t cstate, bool modeled)
{
   bool cache_hit = false;

   switch (shmem_msg_type)
   {
      case ShmemMsg::EX_REQ:
         cache_hit = CacheState(cstate).writable();
         break;

      case ShmemMsg::SH_REQ:
         cache_hit = CacheState(cstate).readable();
         break;

      default:
         LOG_PRINT_ERROR("Unsupported Shmem Msg Type: %u", shmem_msg_type);
         break;
   }

   if (modeled)
   {
      m_l2_cache->updateCounters(cache_hit);
   }

   return cache_hit;
}

MemComponent::component_t
L2CacheCntlr::acquireL1CacheLock(ShmemMsg::msg_t msg_type, IntPtr address)
{
   switch (msg_type)
   {
      case ShmemMsg::EX_REP:
      case ShmemMsg::SH_REP:

         m_l1_cache_cntlr->acquireLock(m_shmem_req_source_map[address]);
         return m_shmem_req_source_map[address];

      case ShmemMsg::INV_REQ:
      case ShmemMsg::FLUSH_REQ:
      case ShmemMsg::WB_REQ:

         {
            acquireLock();

            PrL2CacheBlockInfo* l2_cache_block_info = getCacheBlockInfo(address);
            MemComponent::component_t caching_mem_component = (l2_cache_block_info == NULL) ? MemComponent::INVALID_MEM_COMPONENT : l2_cache_block_info->getCachedLoc();

            releaseLock();

            //WH: Why?//assert(caching_mem_component != MemComponent::L1_ICACHE);
            if (caching_mem_component != MemComponent::INVALID_MEM_COMPONENT)
            {
               m_l1_cache_cntlr->acquireLock(caching_mem_component);
            }
            return caching_mem_component;
         }

      default:
         LOG_PRINT_ERROR("Unrecognized Msg Type (%u)", msg_type);
         return MemComponent::INVALID_MEM_COMPONENT;
   }

}

void
L2CacheCntlr::acquireLock()
{
   m_l2_cache_lock.acquire();
}

void
L2CacheCntlr::releaseLock()
{
   m_l2_cache_lock.release();
}

void
L2CacheCntlr::wakeUpUserThread()
{
   m_user_thread_sem->signal();
}

void
L2CacheCntlr::waitForUserThread()
{
   m_network_thread_sem->wait();
}

}
