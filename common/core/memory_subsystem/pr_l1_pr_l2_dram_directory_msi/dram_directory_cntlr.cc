#include "dram_directory_cntlr.h"
#include "log.h"
#include "memory_manager.h"
#include "stats.h"
#include "nuca_cache.h"

#if 0
   extern Lock iolock;
#  include "core_manager.h"
#  include "simulator.h"
#  define MYLOG(...) { ScopedLock l(iolock); fflush(stdout); printf("[%s] %d%cdd %-25s@%3u: ", itostr(getShmemPerfModel()->getElapsedTime()).c_str(), getMemoryManager()->getCore()->getId(), Sim()->getCoreManager()->amiUserThread() ? '^' : '_', __FUNCTION__, __LINE__); printf(__VA_ARGS__); printf("\n"); fflush(stdout); }
#else
#  define MYLOG(...) {}
#endif

namespace PrL1PrL2DramDirectoryMSI
{

DramDirectoryCntlr::DramDirectoryCntlr(core_id_t core_id,
      MemoryManagerBase* memory_manager,
      AddressHomeLookup* dram_controller_home_lookup,
      NucaCache* nuca_cache,
      UInt32 dram_directory_total_entries,
      UInt32 dram_directory_associativity,
      UInt32 cache_block_size,
      UInt32 dram_directory_max_num_sharers,
      UInt32 dram_directory_max_hw_sharers,
      String dram_directory_type_str,
      SubsecondTime dram_directory_cache_access_time,
      ShmemPerfModel* shmem_perf_model):
   m_memory_manager(memory_manager),
   m_dram_controller_home_lookup(dram_controller_home_lookup),
   m_nuca_cache(nuca_cache),
   m_core_id(core_id),
   m_cache_block_size(cache_block_size),
   m_max_hw_sharers(dram_directory_max_hw_sharers),
   m_shmem_perf_model(shmem_perf_model),
   evict_modified(0),
   evict_shared(0)
{
   m_dram_directory_cache = new DramDirectoryCache(
         core_id,
         dram_directory_type_str,
         dram_directory_total_entries,
         dram_directory_associativity,
         cache_block_size,
         dram_directory_max_hw_sharers,
         dram_directory_max_num_sharers,
         dram_directory_cache_access_time,
         m_shmem_perf_model);
   m_dram_directory_req_queue_list = new ReqQueueList();
   registerStatsMetric("directory", core_id, "evict-modified", &evict_modified);
   registerStatsMetric("directory", core_id, "evict-shared", &evict_shared);
}

DramDirectoryCntlr::~DramDirectoryCntlr()
{
   delete m_dram_directory_cache;
   delete m_dram_directory_req_queue_list;
}

void
DramDirectoryCntlr::handleMsgFromL2Cache(core_id_t sender, ShmemMsg* shmem_msg)
{
   ShmemMsg::msg_t shmem_msg_type = shmem_msg->getMsgType();
   SubsecondTime msg_time = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD);
MYLOG("begin");

   switch (shmem_msg_type)
   {
      case ShmemMsg::EX_REQ:
      case ShmemMsg::SH_REQ:

         {
            IntPtr address = shmem_msg->getAddress();
MYLOG("%c REQ<%u @ %lx", shmem_msg_type == ShmemMsg::EX_REQ ? 'E' : 'S', sender, address);

            // Add request onto a queue
            ShmemReq* shmem_req = new ShmemReq(shmem_msg, msg_time);
            m_dram_directory_req_queue_list->enqueue(address, shmem_req);
            if (m_dram_directory_req_queue_list->size(address) == 1)
            {
               if (shmem_msg_type == ShmemMsg::EX_REQ)
                  processExReqFromL2Cache(shmem_req);
               else if (shmem_msg_type == ShmemMsg::SH_REQ)
                  processShReqFromL2Cache(shmem_req);
               else
                  LOG_PRINT_ERROR("Unrecognized Request(%u)", shmem_msg_type);
            }
         }
         break;

      case ShmemMsg::INV_REP:
MYLOG("INV REP<%u @ %lx", sender, shmem_msg->getAddress());
         processInvRepFromL2Cache(sender, shmem_msg);
         break;

      case ShmemMsg::FLUSH_REP:
MYLOG("FLUSH REP<%u @ %lx", sender, shmem_msg->getAddress());
         processFlushRepFromL2Cache(sender, shmem_msg);
         break;

      case ShmemMsg::WB_REP:
MYLOG("WB REP<%u @ %lx", sender, shmem_msg->getAddress());
         processWbRepFromL2Cache(sender, shmem_msg);
         break;

      default:
         LOG_PRINT_ERROR("Unrecognized Shmem Msg Type: %u", shmem_msg_type);
         break;
   }
MYLOG("done");
}

void
DramDirectoryCntlr::handleMsgFromDRAM(core_id_t sender, ShmemMsg* shmem_msg)
{
   ShmemMsg::msg_t shmem_msg_type = shmem_msg->getMsgType();
   SubsecondTime msg_time = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD);

   switch (shmem_msg_type)
   {
      case ShmemMsg::DRAM_READ_REP:
         processDRAMReply(sender, shmem_msg);
         break;

      default:
         LOG_PRINT_ERROR("Unrecognized Shmem Msg Type: %u", shmem_msg_type);
         break;
   }
}

void
DramDirectoryCntlr::processNextReqFromL2Cache(IntPtr address)
{
   LOG_PRINT("Start processNextReqFromL2Cache(0x%x)", address);

   assert(m_dram_directory_req_queue_list->size(address) >= 1);
   ShmemReq* completed_shmem_req = m_dram_directory_req_queue_list->dequeue(address);
   delete completed_shmem_req;

   if (! m_dram_directory_req_queue_list->empty(address))
   {
      LOG_PRINT("A new shmem req for address(0x%x) found", address);
      ShmemReq* shmem_req = m_dram_directory_req_queue_list->front(address);

      // Update the Shared Mem Cycle Counts appropriately
      getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_SIM_THREAD, shmem_req->getTime());

      if (shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::EX_REQ)
         processExReqFromL2Cache(shmem_req);
      else if (shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::SH_REQ)
         processShReqFromL2Cache(shmem_req);
      else
         LOG_PRINT_ERROR("Unrecognized Request(%u)", shmem_req->getShmemMsg()->getMsgType());
   }
   LOG_PRINT("End processNextReqFromL2Cache(0x%x)", address);
}

DirectoryEntry*
DramDirectoryCntlr::processDirectoryEntryAllocationReq(ShmemReq* shmem_req)
{
   IntPtr address = shmem_req->getShmemMsg()->getAddress();
   core_id_t requester = shmem_req->getShmemMsg()->getRequester();
   SubsecondTime msg_time = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD);

   std::vector<DirectoryEntry*> replacement_candidate_list;
   m_dram_directory_cache->getReplacementCandidates(address, replacement_candidate_list);

   std::vector<DirectoryEntry*>::iterator it;
   std::vector<DirectoryEntry*>::iterator replacement_candidate = replacement_candidate_list.end();
   for (it = replacement_candidate_list.begin(); it != replacement_candidate_list.end(); it++)
   {
      if ( ( (replacement_candidate == replacement_candidate_list.end()) ||
             ((*replacement_candidate)->getNumSharers() > (*it)->getNumSharers())
           )
           &&
           (m_dram_directory_req_queue_list->size((*it)->getAddress()) == 0)
         )
      {
         replacement_candidate = it;
      }
   }

   LOG_ASSERT_ERROR(replacement_candidate != replacement_candidate_list.end(),
         "Cant find a directory entry to be replaced with a non-zero request list");

   switch(DirectoryState::dstate_t curr_dstate = (*replacement_candidate)->getDirectoryBlockInfo()->getDState()) {
      case DirectoryState::MODIFIED:
         evict_modified++;
         break;
      case DirectoryState::SHARED:
         evict_shared++;
         break;
      case DirectoryState::UNCACHED:
         break;
      default:
         LOG_PRINT_ERROR("Unsupported Directory State: %u", curr_dstate);
         break;
   }

   IntPtr replaced_address = (*replacement_candidate)->getAddress();

   // We get the entry with the lowest number of sharers
   DirectoryEntry* directory_entry = m_dram_directory_cache->replaceDirectoryEntry(replaced_address, address);

   ShmemMsg nullify_msg(ShmemMsg::NULLIFY_REQ, MemComponent::TAG_DIR, MemComponent::TAG_DIR, requester, replaced_address, NULL, 0);

   ShmemReq* nullify_req = new ShmemReq(&nullify_msg, msg_time);
   m_dram_directory_req_queue_list->enqueue(replaced_address, nullify_req);

   assert(m_dram_directory_req_queue_list->size(replaced_address) == 1);
   processNullifyReq(nullify_req);

   return directory_entry;
}

void
DramDirectoryCntlr::processNullifyReq(ShmemReq* shmem_req)
{
   IntPtr address = shmem_req->getShmemMsg()->getAddress();
   core_id_t requester = shmem_req->getShmemMsg()->getRequester();

   DirectoryEntry* directory_entry = m_dram_directory_cache->getDirectoryEntry(address);
   assert(directory_entry);

   DirectoryBlockInfo* directory_block_info = directory_entry->getDirectoryBlockInfo();
   DirectoryState::dstate_t curr_dstate = directory_block_info->getDState();

   switch (curr_dstate)
   {
      case DirectoryState::MODIFIED:
         getMemoryManager()->sendMsg(ShmemMsg::FLUSH_REQ,
               MemComponent::TAG_DIR, MemComponent::L2_CACHE,
               requester /* requester */,
               directory_entry->getOwner() /* receiver */,
               address,
               NULL, 0,
               HitWhere::UNKNOWN, ShmemPerfModel::_SIM_THREAD);
         break;

      case DirectoryState::SHARED:

         {
            std::pair<bool, std::vector<SInt32> > sharers_list_pair = directory_entry->getSharersList();
            if (sharers_list_pair.first == true)
            {
               // Broadcast Invalidation Request to all cores
               // (irrespective of whether they are sharers or not)
               getMemoryManager()->broadcastMsg(ShmemMsg::INV_REQ,
                     MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                     requester /* requester */,
                     address,
                     NULL, 0,
                     ShmemPerfModel::_SIM_THREAD);
            }
            else
            {
               // Send Invalidation Request to only a specific set of sharers
               for (UInt32 i = 0; i < sharers_list_pair.second.size(); i++)
               {
                  getMemoryManager()->sendMsg(ShmemMsg::INV_REQ,
                        MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                        requester /* requester */,
                        sharers_list_pair.second[i] /* receiver */,
                        address,
                        NULL, 0,
                        HitWhere::UNKNOWN, ShmemPerfModel::_SIM_THREAD);
               }
            }
         }
         break;

      case DirectoryState::UNCACHED:

         {
            m_dram_directory_cache->invalidateDirectoryEntry(address);

            // Process Next Request
            processNextReqFromL2Cache(address);
         }
         break;

      default:
         LOG_PRINT_ERROR("Unsupported Directory State: %u", curr_dstate);
         break;
   }

}

void
DramDirectoryCntlr::processExReqFromL2Cache(ShmemReq* shmem_req, Byte* cached_data_buf)
{
   IntPtr address = shmem_req->getShmemMsg()->getAddress();
   core_id_t requester = shmem_req->getShmemMsg()->getRequester();

   DirectoryEntry* directory_entry = m_dram_directory_cache->getDirectoryEntry(address);
   if (directory_entry == NULL)
   {
      directory_entry = processDirectoryEntryAllocationReq(shmem_req);
   }

   DirectoryBlockInfo* directory_block_info = directory_entry->getDirectoryBlockInfo();
   DirectoryState::dstate_t curr_dstate = directory_block_info->getDState();

   switch (curr_dstate)
   {
      case DirectoryState::MODIFIED:
         assert(cached_data_buf == NULL);
         getMemoryManager()->sendMsg(ShmemMsg::FLUSH_REQ,
               MemComponent::TAG_DIR, MemComponent::L2_CACHE,
               requester /* requester */,
               directory_entry->getOwner() /* receiver */,
               address,
               NULL, 0,
               HitWhere::UNKNOWN, ShmemPerfModel::_SIM_THREAD);
         break;

      case DirectoryState::SHARED:

         {
            assert(cached_data_buf == NULL);
            std::pair<bool, std::vector<SInt32> > sharers_list_pair = directory_entry->getSharersList();
            if (sharers_list_pair.first == true)
            {
               // Broadcast Invalidation Request to all cores
               // (irrespective of whether they are sharers or not)
               getMemoryManager()->broadcastMsg(ShmemMsg::INV_REQ,
                     MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                     requester /* requester */,
                     address,
                     NULL, 0,
                     ShmemPerfModel::_SIM_THREAD);
            }
            else
            {
               // Send Invalidation Request to only a specific set of sharers
               for (UInt32 i = 0; i < sharers_list_pair.second.size(); i++)
               {
                  getMemoryManager()->sendMsg(ShmemMsg::INV_REQ,
                        MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                        requester /* requester */,
                        sharers_list_pair.second[i] /* receiver */,
                        address,
                        NULL, 0,
                        HitWhere::UNKNOWN, ShmemPerfModel::_SIM_THREAD);
               }
            }
         }
         break;

      case DirectoryState::UNCACHED:

         {
            // Modifiy the directory entry contents
            bool add_result = directory_entry->addSharer(requester, m_max_hw_sharers);
            assert(add_result == true);
            directory_entry->setOwner(requester);
            directory_block_info->setDState(DirectoryState::MODIFIED);

            retrieveDataAndSendToL2Cache(ShmemMsg::EX_REP, requester, address, cached_data_buf);
         }
         break;

      default:
         LOG_PRINT_ERROR("Unsupported Directory State: %u", curr_dstate);
         break;
   }
}

void
DramDirectoryCntlr::processShReqFromL2Cache(ShmemReq* shmem_req, Byte* cached_data_buf)
{
   IntPtr address = shmem_req->getShmemMsg()->getAddress();
   core_id_t requester = shmem_req->getShmemMsg()->getRequester();

   DirectoryEntry* directory_entry = m_dram_directory_cache->getDirectoryEntry(address);
   if (directory_entry == NULL)
   {
      directory_entry = processDirectoryEntryAllocationReq(shmem_req);
   }

   DirectoryBlockInfo* directory_block_info = directory_entry->getDirectoryBlockInfo();
   DirectoryState::dstate_t curr_dstate = directory_block_info->getDState();

   switch (curr_dstate)
   {
      case DirectoryState::MODIFIED:
         {
            assert(cached_data_buf == NULL);
            getMemoryManager()->sendMsg(ShmemMsg::WB_REQ,
                  MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                  requester /* requester */,
                  directory_entry->getOwner() /* receiver */,
                  address,
                  NULL, 0,
                  HitWhere::UNKNOWN, ShmemPerfModel::_SIM_THREAD);
         }
         break;

      case DirectoryState::SHARED:
         {
            bool add_result = directory_entry->addSharer(requester, m_max_hw_sharers);
            if (add_result == false)
            {
               core_id_t sharer_id = directory_entry->getOneSharer();
               // Send a message to another sharer to invalidate that
               getMemoryManager()->sendMsg(ShmemMsg::INV_REQ,
                     MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                     requester /* requester */,
                     sharer_id /* receiver */,
                     address,
                     NULL, 0,
                     HitWhere::UNKNOWN, ShmemPerfModel::_SIM_THREAD);
            }
            else
            {
               retrieveDataAndSendToL2Cache(ShmemMsg::SH_REP, requester, address, cached_data_buf);
            }
         }
         break;

      case DirectoryState::UNCACHED:
         {
            // Modifiy the directory entry contents
            bool add_result = directory_entry->addSharer(requester, m_max_hw_sharers);
            assert(add_result == true);
            directory_block_info->setDState(DirectoryState::SHARED);

            retrieveDataAndSendToL2Cache(ShmemMsg::SH_REP, requester, address, cached_data_buf);
         }
         break;

      default:
         LOG_PRINT_ERROR("Unsupported Directory State: %u", curr_dstate);
         break;
   }
}

void
DramDirectoryCntlr::retrieveDataAndSendToL2Cache(ShmemMsg::msg_t reply_msg_type,
      core_id_t receiver, IntPtr address, Byte* cached_data_buf)
{
   if (cached_data_buf != NULL)
   {
      // I already have the data I need cached
      getMemoryManager()->sendMsg(reply_msg_type,
            MemComponent::TAG_DIR, MemComponent::L2_CACHE,
            receiver /* requester */,
            receiver /* receiver */,
            address,
            cached_data_buf, getCacheBlockSize(),
            HitWhere::CACHE_REMOTE /* cached_data_buf was filled by a WB_REQ or FLUSH_REQ */,
            ShmemPerfModel::_SIM_THREAD);

      // Process Next Request
      processNextReqFromL2Cache(address);
   }
   else
   {
      if (m_nuca_cache)
      {
         SubsecondTime nuca_latency;
         HitWhere::where_t hit_where;
         Byte nuca_data_buf[getCacheBlockSize()];
         boost::tie(nuca_latency, hit_where) = m_nuca_cache->read(address, nuca_data_buf, getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD));

         getShmemPerfModel()->incrElapsedTime(nuca_latency, ShmemPerfModel::_SIM_THREAD);

         if (hit_where != HitWhere::MISS)
         {
            getMemoryManager()->sendMsg(reply_msg_type,
                  MemComponent::TAG_DIR, MemComponent::L2_CACHE,
                  receiver /* requester */,
                  receiver /* receiver */,
                  address,
                  nuca_data_buf, getCacheBlockSize(),
                  HitWhere::NUCA_CACHE,
                  ShmemPerfModel::_SIM_THREAD);

            // Process Next Request
            processNextReqFromL2Cache(address);

            return;
         }
      }

      // Get the data from DRAM
      // This could be directly forwarded to the cache or passed
      // through the Dram Directory Controller

      assert(m_dram_directory_req_queue_list->size(address) > 0);
      ShmemReq* shmem_req = m_dram_directory_req_queue_list->front(address);
      // Remember that this request is waiting for data, and should not be woken up by voluntary invalidates
      shmem_req->setWaitForData(true);

      core_id_t dram_node = m_dram_controller_home_lookup->getHome(address);

      getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::DRAM_READ_REQ,
            MemComponent::TAG_DIR, MemComponent::DRAM,
            receiver /* requester */,
            dram_node /* receiver */,
            address,
            NULL, 0,
            HitWhere::UNKNOWN,
            ShmemPerfModel::_SIM_THREAD);
   }
}

void
DramDirectoryCntlr::processDRAMReply(core_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();

   // Data received from DRAM

   //   Which node to reply to?

   assert(m_dram_directory_req_queue_list->size(address) >= 1);
   ShmemReq* shmem_req = m_dram_directory_req_queue_list->front(address);

   //   Which reply type to use?

   ShmemMsg::msg_t reply_msg_type;
   DirectoryEntry* directory_entry = m_dram_directory_cache->getDirectoryEntry(address);
   assert(directory_entry);
   DirectoryBlockInfo* directory_block_info = directory_entry->getDirectoryBlockInfo();
   DirectoryState::dstate_t curr_dstate = directory_block_info->getDState();

   switch(shmem_req->getShmemMsg()->getMsgType())
   {
      case ShmemMsg::SH_REQ:
         reply_msg_type = ShmemMsg::SH_REP;
         assert(curr_dstate == DirectoryState::SHARED);
         break;
      case ShmemMsg::EX_REQ:
         reply_msg_type = ShmemMsg::EX_REP;
         assert(curr_dstate == DirectoryState::MODIFIED);
         break;
      default:
         LOG_PRINT_ERROR("Unsupported request type: %u", shmem_req->getShmemMsg()->getMsgType());
   }

   //   Which HitWhere to report?

   HitWhere::where_t hit_where = shmem_msg->getWhere();
   if (hit_where == HitWhere::DRAM)
      hit_where = (sender == shmem_msg->getRequester()) ? HitWhere::DRAM_LOCAL : HitWhere::DRAM_REMOTE;

   //   Send reply

   getMemoryManager()->sendMsg(reply_msg_type,
         MemComponent::TAG_DIR, MemComponent::L2_CACHE,
         shmem_req->getShmemMsg()->getRequester() /* requester */,
         shmem_req->getShmemMsg()->getRequester() /* receiver */,
         address,
         shmem_msg->getDataBuf(), getCacheBlockSize(),
         hit_where,
         ShmemPerfModel::_SIM_THREAD);

   // Keep a copy in NUCA
   sendDataToNUCA(address, shmem_req->getShmemMsg()->getRequester(), shmem_msg->getDataBuf(), getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD));

   // Process Next Request
   processNextReqFromL2Cache(address);
}

void
DramDirectoryCntlr::processInvRepFromL2Cache(core_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();

   DirectoryEntry* directory_entry = m_dram_directory_cache->getDirectoryEntry(address);
   assert(directory_entry);

   DirectoryBlockInfo* directory_block_info = directory_entry->getDirectoryBlockInfo();
   assert(directory_block_info->getDState() == DirectoryState::SHARED);

   directory_entry->removeSharer(sender);
   if (directory_entry->getNumSharers() == 0)
   {
      directory_block_info->setDState(DirectoryState::UNCACHED);
   }

   if (m_dram_directory_req_queue_list->size(address) > 0)
   {
      ShmemReq* shmem_req = m_dram_directory_req_queue_list->front(address);

      // Update Times in the Shmem Perf Model and the Shmem Req
      shmem_req->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD));
      getShmemPerfModel()->updateElapsedTime(shmem_req->getTime(), ShmemPerfModel::_SIM_THREAD);

      if (shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::EX_REQ)
      {
         // An ShmemMsg::EX_REQ caused the invalidation
         if (directory_block_info->getDState() == DirectoryState::UNCACHED)
         {
            processExReqFromL2Cache(shmem_req);
         }
      }
      else if (shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::SH_REQ)
      {
         if (shmem_req->getWaitForData() == false)
         {
            // A ShmemMsg::SH_REQ caused the invalidation
            processShReqFromL2Cache(shmem_req);
         }
         else
         {
            // This is a voluntary invalidate (probably part of an upgrade),
            // the next request should only be woken up once its data arrives.
         }
      }
      else // shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::NULLIFY_REQ
      {
         if (directory_block_info->getDState() == DirectoryState::UNCACHED)
         {
            processNullifyReq(shmem_req);
         }
      }
   }
}

void
DramDirectoryCntlr::processFlushRepFromL2Cache(core_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
   SubsecondTime now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD);

   DirectoryEntry* directory_entry = m_dram_directory_cache->getDirectoryEntry(address);
   assert(directory_entry);

   DirectoryBlockInfo* directory_block_info = directory_entry->getDirectoryBlockInfo();
   assert(directory_block_info->getDState() == DirectoryState::MODIFIED);

   directory_entry->removeSharer(sender);
   directory_entry->setOwner(INVALID_CORE_ID);
   directory_block_info->setDState(DirectoryState::UNCACHED);

   if (m_dram_directory_req_queue_list->size(address) != 0)
   {
      ShmemReq* shmem_req = m_dram_directory_req_queue_list->front(address);

      // Update times
      shmem_req->updateTime(now);
      getShmemPerfModel()->updateElapsedTime(shmem_req->getTime(), ShmemPerfModel::_SIM_THREAD);

      // An involuntary/voluntary Flush
      if (shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::EX_REQ)
      {
         processExReqFromL2Cache(shmem_req, shmem_msg->getDataBuf());
      }
      else if (shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::SH_REQ)
      {
         // Write Data to Dram
         sendDataToDram(address, shmem_msg->getRequester(), shmem_msg->getDataBuf(), now);
         processShReqFromL2Cache(shmem_req, shmem_msg->getDataBuf());
      }
      else // shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::NULLIFY_REQ
      {
         // Write Data To Dram
         sendDataToDram(address, shmem_msg->getRequester(), shmem_msg->getDataBuf(), now);
         processNullifyReq(shmem_req);
      }
   }
   else
   {
      // This was just an eviction
      // Write Data to Dram
      sendDataToDram(address, shmem_msg->getRequester(), shmem_msg->getDataBuf(), now);
   }
}

void
DramDirectoryCntlr::processWbRepFromL2Cache(core_id_t sender, ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
   SubsecondTime now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD);

   DirectoryEntry* directory_entry = m_dram_directory_cache->getDirectoryEntry(address);
   assert(directory_entry);

   DirectoryBlockInfo* directory_block_info = directory_entry->getDirectoryBlockInfo();

   assert(directory_block_info->getDState() == DirectoryState::MODIFIED);
   assert(directory_entry->hasSharer(sender));

   directory_entry->setOwner(INVALID_CORE_ID);
   directory_block_info->setDState(DirectoryState::SHARED);

   if (m_dram_directory_req_queue_list->size(address) != 0)
   {
      ShmemReq* shmem_req = m_dram_directory_req_queue_list->front(address);

      // Update Time
      shmem_req->updateTime(now);
      getShmemPerfModel()->updateElapsedTime(shmem_req->getTime(), ShmemPerfModel::_SIM_THREAD);

      // Write Data to Dram
      sendDataToDram(address, shmem_msg->getRequester(), shmem_msg->getDataBuf(), now);

      LOG_ASSERT_ERROR(shmem_req->getShmemMsg()->getMsgType() == ShmemMsg::SH_REQ,
            "Address(0x%x), Req(%u)",
            address, shmem_req->getShmemMsg()->getMsgType());
      processShReqFromL2Cache(shmem_req, shmem_msg->getDataBuf());
   }
   else
   {
      LOG_PRINT_ERROR("Should not reach here");
   }
}

void
DramDirectoryCntlr::sendDataToNUCA(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now)
{
   if (m_nuca_cache)
   {
      bool eviction;
      IntPtr evict_address;
      Byte evict_buf[getCacheBlockSize()];

      m_nuca_cache->write(
         address, data_buf,
         eviction, evict_address, evict_buf,
         getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD)
      );

      if (eviction)
      {
         // Write data to Dram
         core_id_t dram_node = m_dram_controller_home_lookup->getHome(evict_address);

         getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::DRAM_WRITE_REQ,
               MemComponent::TAG_DIR, MemComponent::DRAM,
               m_core_id /* requester */,
               dram_node /* receiver */,
               evict_address,
               evict_buf, getCacheBlockSize(),
               HitWhere::UNKNOWN,
               ShmemPerfModel::_SIM_THREAD);
      }
   }
}

void
DramDirectoryCntlr::sendDataToDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now)
{
   if (m_nuca_cache)
   {
      // If we have a NUCA cache: write it there, it will be written to DRAM on eviction

      sendDataToNUCA(address, requester, data_buf, now);
   }
   else
   {
      // Write data to Dram
      core_id_t dram_node = m_dram_controller_home_lookup->getHome(address);

      getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::DRAM_WRITE_REQ,
            MemComponent::TAG_DIR, MemComponent::DRAM,
            requester /* requester */,
            dram_node /* receiver */,
            address,
            data_buf, getCacheBlockSize(),
            HitWhere::UNKNOWN,
            ShmemPerfModel::_SIM_THREAD);

      // DRAM latency is ignored on write
   }
}

}
