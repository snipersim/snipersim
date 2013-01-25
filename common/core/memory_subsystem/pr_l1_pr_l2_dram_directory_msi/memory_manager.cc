#include "memory_manager.h"
#include "cache_base.h"
#include "simulator.h"
#include "log.h"
#include "dvfs_manager.h"
#include "config.hpp"
#include "distribution.h"

namespace PrL1PrL2DramDirectoryMSI
{

MemoryManager::MemoryManager(Core* core,
      Network* network, ShmemPerfModel* shmem_perf_model):
   MemoryManagerBase(core, network, shmem_perf_model),
   m_dram_directory_cntlr(NULL),
   m_dram_cntlr(NULL),
   m_dram_cntlr_present(false),
   m_enabled(false)
{
   UInt32 l1_icache_size = 0;
   UInt32 l1_icache_associativity = 0;
   String l1_icache_replacement_policy;
   ComponentLatency l1_icache_data_access_time(NULL,0);
   ComponentLatency l1_icache_tags_access_time(NULL,0);
   String l1_icache_perf_model_type;

   UInt32 l1_dcache_size = 0;
   UInt32 l1_dcache_associativity = 0;
   String l1_dcache_replacement_policy;
   ComponentLatency l1_dcache_data_access_time(NULL,0);
   ComponentLatency l1_dcache_tags_access_time(NULL,0);
   String l1_dcache_perf_model_type;

   UInt32 l2_cache_size = 0;
   UInt32 l2_cache_associativity = 0;
   String l2_cache_replacement_policy;
   ComponentLatency l2_cache_data_access_time(NULL,0);
   ComponentLatency l2_cache_tags_access_time(NULL,0);
   String l2_cache_perf_model_type;

   UInt32 dram_directory_total_entries = 0;
   UInt32 dram_directory_associativity = 0;
   UInt32 dram_directory_max_num_sharers = 0;
   UInt32 dram_directory_max_hw_sharers = 0;
   String dram_directory_type_str;
   UInt32 dram_directory_home_lookup_param = 0;
   SubsecondTime dram_directory_cache_access_time(SubsecondTime::Zero());

   try
   {
      m_cache_block_size = Sim()->getCfg()->getInt("perf_model/l1_icache/cache_block_size");

      // L1 ICache
      l1_icache_size = Sim()->getCfg()->getInt("perf_model/l1_icache/cache_size");
      l1_icache_associativity = Sim()->getCfg()->getInt("perf_model/l1_icache/associativity");
      l1_icache_replacement_policy = Sim()->getCfg()->getString("perf_model/l1_icache/replacement_policy");
      l1_icache_data_access_time = ComponentLatency(core->getDvfsDomain(), Sim()->getCfg()->getInt("perf_model/l1_icache/data_access_time"));
      l1_icache_tags_access_time = ComponentLatency(core->getDvfsDomain(), Sim()->getCfg()->getInt("perf_model/l1_icache/tags_access_time"));
      l1_icache_perf_model_type = Sim()->getCfg()->getString("perf_model/l1_icache/perf_model_type");

      // L1 DCache
      l1_dcache_size = Sim()->getCfg()->getInt("perf_model/l1_dcache/cache_size");
      l1_dcache_associativity = Sim()->getCfg()->getInt("perf_model/l1_dcache/associativity");
      l1_dcache_replacement_policy = Sim()->getCfg()->getString("perf_model/l1_dcache/replacement_policy");
      l1_dcache_data_access_time = ComponentLatency(core->getDvfsDomain(), Sim()->getCfg()->getInt("perf_model/l1_dcache/data_access_time"));
      l1_dcache_tags_access_time = ComponentLatency(core->getDvfsDomain(), Sim()->getCfg()->getInt("perf_model/l1_dcache/tags_access_time"));
      l1_dcache_perf_model_type = Sim()->getCfg()->getString("perf_model/l1_dcache/perf_model_type");

      // L2 Cache
      l2_cache_size = Sim()->getCfg()->getInt("perf_model/l2_cache/cache_size");
      l2_cache_associativity = Sim()->getCfg()->getInt("perf_model/l2_cache/associativity");
      l2_cache_replacement_policy = Sim()->getCfg()->getString("perf_model/l2_cache/replacement_policy");
      l2_cache_data_access_time = ComponentLatency(core->getDvfsDomain(), Sim()->getCfg()->getInt("perf_model/l2_cache/data_access_time"));
      l2_cache_tags_access_time = ComponentLatency(core->getDvfsDomain(), Sim()->getCfg()->getInt("perf_model/l2_cache/tags_access_time"));
      l2_cache_perf_model_type = Sim()->getCfg()->getString("perf_model/l2_cache/perf_model_type");

      // Dram Directory Cache
      dram_directory_total_entries = Sim()->getCfg()->getInt("perf_model/dram_directory/total_entries");
      dram_directory_associativity = Sim()->getCfg()->getInt("perf_model/dram_directory/associativity");
      dram_directory_max_num_sharers = Sim()->getConfig()->getTotalCores();
      dram_directory_max_hw_sharers = Sim()->getCfg()->getInt("perf_model/dram_directory/max_hw_sharers");
      dram_directory_type_str = Sim()->getCfg()->getString("perf_model/dram_directory/directory_type");
      dram_directory_home_lookup_param = Sim()->getCfg()->getInt("perf_model/dram_directory/home_lookup_param");
      dram_directory_cache_access_time = SubsecondTime::NS() * Sim()->getCfg()->getInt("perf_model/dram_directory/directory_cache_access_time");
   }
   catch(...)
   {
      LOG_PRINT_ERROR("Error reading memory system parameters from the config file");
   }

   m_user_thread_sem = new Semaphore(0);
   m_network_thread_sem = new Semaphore(0);

   std::vector<core_id_t> core_list_with_dram_controllers = getCoreListWithMemoryControllers();

   // if (m_core->getId() == 0)
   //   printCoreListWithMemoryControllers(core_list_with_dram_controllers);

   if (find(core_list_with_dram_controllers.begin(), core_list_with_dram_controllers.end(), getCore()->getId()) != core_list_with_dram_controllers.end())
   {
      m_dram_cntlr_present = true;

      m_dram_cntlr = new DramCntlr(this,
            getCacheBlockSize());

      m_dram_directory_cntlr = new DramDirectoryCntlr(getCore()->getId(),
            this,
            m_dram_cntlr,
            dram_directory_total_entries,
            dram_directory_associativity,
            getCacheBlockSize(),
            dram_directory_max_num_sharers,
            dram_directory_max_hw_sharers,
            dram_directory_type_str,
            dram_directory_cache_access_time,
            getShmemPerfModel());
   }

   m_dram_directory_home_lookup = new AddressHomeLookup(dram_directory_home_lookup_param, core_list_with_dram_controllers, getCacheBlockSize());

   m_l1_cache_cntlr = new L1CacheCntlr(getCore()->getId(),
         this,
         m_user_thread_sem,
         m_network_thread_sem,
         getCacheBlockSize(),
         l1_icache_size, l1_icache_associativity,
         l1_icache_replacement_policy,
         l1_dcache_size, l1_dcache_associativity,
         l1_dcache_replacement_policy,
         getShmemPerfModel());

   m_l2_cache_cntlr = new L2CacheCntlr(getCore()->getId(),
         this,
         m_l1_cache_cntlr,
         m_dram_directory_home_lookup,
         m_user_thread_sem,
         m_network_thread_sem,
         getCacheBlockSize(),
         l2_cache_size, l2_cache_associativity,
         l2_cache_replacement_policy,
         getShmemPerfModel());

   m_l1_cache_cntlr->setL2CacheCntlr(m_l2_cache_cntlr);

   // Create Performance Models
   m_l1_icache_perf_model = CachePerfModel::create(l1_icache_perf_model_type,
         l1_icache_data_access_time, l1_icache_tags_access_time);
   m_l1_dcache_perf_model = CachePerfModel::create(l1_dcache_perf_model_type,
         l1_dcache_data_access_time, l1_dcache_tags_access_time);
   m_l2_cache_perf_model = CachePerfModel::create(l2_cache_perf_model_type,
         l2_cache_data_access_time, l2_cache_tags_access_time);

   // Register Call-backs
   getNetwork()->registerCallback(SHARED_MEM_1, MemoryManagerNetworkCallback, this);
}

MemoryManager::~MemoryManager()
{
   getNetwork()->unregisterCallback(SHARED_MEM_1);

   // Delete the Models
   delete m_l1_icache_perf_model;
   delete m_l1_dcache_perf_model;
   delete m_l2_cache_perf_model;

   delete m_user_thread_sem;
   delete m_network_thread_sem;
   delete m_dram_directory_home_lookup;
   delete m_l1_cache_cntlr;
   delete m_l2_cache_cntlr;
   if (m_dram_cntlr_present)
   {
      delete m_dram_cntlr;
      delete m_dram_directory_cntlr;
   }
}

HitWhere::where_t
MemoryManager::coreInitiateMemoryAccess(
      MemComponent::component_t mem_component,
      Core::lock_signal_t lock_signal,
      Core::mem_op_t mem_op_type,
      IntPtr address, UInt32 offset,
      Byte* data_buf, UInt32 data_length,
      Core::MemModeled modeled)
{
   static char buf[1024];
   assert(data_buf || data_length < 1024); /* IF we use buf, make sure it's big enough */

   bool hit = m_l1_cache_cntlr->processMemOpFromCore(mem_component,
         lock_signal,
         mem_op_type,
         address, offset,
         data_buf ? data_buf : (Byte*)buf, data_length,
         modeled == Core::MEM_MODELED_NONE ? false : true);
   if (hit)
      return (HitWhere::where_t)mem_component;
   else
      return HitWhere::MISS;
}

void
MemoryManager::handleMsgFromNetwork(NetPacket& packet)
{
   core_id_t sender = packet.sender;
   ShmemMsg* shmem_msg = ShmemMsg::getShmemMsg((Byte*) packet.data);
   SubsecondTime msg_time = packet.time;

   getShmemPerfModel()->setElapsedTime(msg_time);

   MemComponent::component_t receiver_mem_component = shmem_msg->getReceiverMemComponent();
   MemComponent::component_t sender_mem_component = shmem_msg->getSenderMemComponent();

   if (m_enabled)
   {
      LOG_PRINT("Got Shmem Msg: type(%i), address(0x%x), sender_mem_component(%u), receiver_mem_component(%u), sender(%i), receiver(%i)",
            shmem_msg->getMsgType(), shmem_msg->getAddress(), sender_mem_component, receiver_mem_component, sender, packet.receiver);
   }

   switch (receiver_mem_component)
   {
      case MemComponent::L2_CACHE:
         switch(sender_mem_component)
         {
            case MemComponent::L1_ICACHE:
            case MemComponent::L1_DCACHE:
               assert(sender == getCore()->getId());
               m_l2_cache_cntlr->handleMsgFromL1Cache(shmem_msg);
               break;

            case MemComponent::DRAM_DIR:
               m_l2_cache_cntlr->handleMsgFromDramDirectory(sender, shmem_msg);
               break;

            default:
               LOG_PRINT_ERROR("Unrecognized sender component(%u)",
                     sender_mem_component);
               break;
         }
         break;

      case MemComponent::DRAM_DIR:
         switch(sender_mem_component)
         {
            LOG_ASSERT_ERROR(m_dram_cntlr_present, "Dram Cntlr NOT present");

            case MemComponent::L2_CACHE:
               m_dram_directory_cntlr->handleMsgFromL2Cache(sender, shmem_msg);
               break;

            default:
               LOG_PRINT_ERROR("Unrecognized sender component(%u)",
                     sender_mem_component);
               break;
         }
         break;

      default:
         LOG_PRINT_ERROR("Unrecognized receiver component(%u)",
               receiver_mem_component);
         break;
   }

   // Delete the allocated Shared Memory Message
   // First delete 'data_buf' if it is present
   // LOG_PRINT("Finished handling Shmem Msg");

   if (shmem_msg->getDataLength() > 0)
   {
      assert(shmem_msg->getDataBuf());
      delete [] shmem_msg->getDataBuf();
   }
   delete shmem_msg;
}

void
MemoryManager::sendMsg(ShmemMsg::msg_t msg_type, MemComponent::component_t sender_mem_component, MemComponent::component_t receiver_mem_component, core_id_t requester, core_id_t receiver, IntPtr address, Byte* data_buf, UInt32 data_length, HitWhere::where_t where, ShmemPerfModel::Thread_t thread_num)
{
   assert((data_buf == NULL) == (data_length == 0));
   ShmemMsg shmem_msg(msg_type, sender_mem_component, receiver_mem_component, requester, address, data_buf, data_length);
   shmem_msg.setWhere(where);

   Byte* msg_buf = shmem_msg.makeMsgBuf();
   SubsecondTime msg_time = getShmemPerfModel()->getElapsedTime();

   if (m_enabled)
   {
      LOG_PRINT("Sending Msg: type(%u), address(0x%x), sender_mem_component(%u), receiver_mem_component(%u), requester(%i), sender(%i), receiver(%i)", msg_type, address, sender_mem_component, receiver_mem_component, requester, getCore()->getId(), receiver);
   }

   NetPacket packet(msg_time, SHARED_MEM_1,
         getCore()->getId(), receiver,
         shmem_msg.getMsgLen(), (const void*) msg_buf);
   getNetwork()->netSend(packet);

   // Delete the Msg Buf
   delete [] msg_buf;
}

void
MemoryManager::broadcastMsg(ShmemMsg::msg_t msg_type, MemComponent::component_t sender_mem_component, MemComponent::component_t receiver_mem_component, core_id_t requester, IntPtr address, Byte* data_buf, UInt32 data_length, ShmemPerfModel::Thread_t thread_num)
{
   assert((data_buf == NULL) == (data_length == 0));
   ShmemMsg shmem_msg(msg_type, sender_mem_component, receiver_mem_component, requester, address, data_buf, data_length);

   Byte* msg_buf = shmem_msg.makeMsgBuf();
   SubsecondTime msg_time = getShmemPerfModel()->getElapsedTime();

   if (m_enabled)
   {
      LOG_PRINT("Sending Msg: type(%u), address(0x%x), sender_mem_component(%u), receiver_mem_component(%u), requester(%i), sender(%i), receiver(%i)", msg_type, address, sender_mem_component, receiver_mem_component, requester, getCore()->getId(), NetPacket::BROADCAST);
   }

   NetPacket packet(msg_time, SHARED_MEM_1,
         getCore()->getId(), NetPacket::BROADCAST,
         shmem_msg.getMsgLen(), (const void*) msg_buf);
   getNetwork()->netSend(packet);

   // Delete the Msg Buf
   delete [] msg_buf;
}

void
MemoryManager::incrElapsedTime(MemComponent::component_t mem_component, CachePerfModel::CacheAccess_t access_type)
{
   switch (mem_component)
   {
      case MemComponent::L1_ICACHE:
         getShmemPerfModel()->incrElapsedTime(m_l1_icache_perf_model->getLatency(access_type));
         break;

      case MemComponent::L1_DCACHE:
         getShmemPerfModel()->incrElapsedTime(m_l1_dcache_perf_model->getLatency(access_type));
         break;

      case MemComponent::L2_CACHE:
         getShmemPerfModel()->incrElapsedTime(m_l2_cache_perf_model->getLatency(access_type));
         break;

      case MemComponent::INVALID_MEM_COMPONENT:
         break;

      default:
         LOG_PRINT_ERROR("Unrecognized mem component type(%u)", mem_component);
         break;
   }
}

void
MemoryManager::enableModels()
{
   m_enabled = true;

   m_l1_cache_cntlr->getL1ICache()->enable();
   m_l1_icache_perf_model->enable();

   m_l1_cache_cntlr->getL1DCache()->enable();
   m_l1_dcache_perf_model->enable();

   m_l2_cache_cntlr->getL2Cache()->enable();
   m_l2_cache_perf_model->enable();

   if (m_dram_cntlr_present)
      m_dram_cntlr->getDramPerfModel()->enable();
}

void
MemoryManager::disableModels()
{
   m_enabled = false;

   m_l1_cache_cntlr->getL1ICache()->disable();
   m_l1_icache_perf_model->disable();

   m_l1_cache_cntlr->getL1DCache()->disable();
   m_l1_dcache_perf_model->disable();

   m_l2_cache_cntlr->getL2Cache()->disable();
   m_l2_cache_perf_model->disable();

   if (m_dram_cntlr_present)
      m_dram_cntlr->getDramPerfModel()->disable();
}

}
