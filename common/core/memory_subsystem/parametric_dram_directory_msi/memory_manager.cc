#include "core_manager.h"
#include "memory_manager.h"
#include "cache_base.h"
#include "dram_cache.h"
#include "simulator.h"
#include "log.h"
#include "dvfs_manager.h"
#include "itostr.h"
#include "instruction.h"
#include "performance_model.h"
#include "config.hpp"

#if 0
   extern Lock iolock;
#  include "core_manager.h"
#  include "simulator.h"
#  define MYLOG(...) { ScopedLock l(iolock); fflush(stderr); fprintf(stderr, "[%s] %d%cmm %-25s@%03u: ", itostr(getShmemPerfModel()->getElapsedTime()).c_str(), getCore()->getId(), Sim()->getCoreManager()->amiUserThread() ? '^' : '_', __FUNCTION__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); }
#else
#  define MYLOG(...) {}
#endif


namespace ParametricDramDirectoryMSI
{

std::map<CoreComponentType, CacheCntlr*> MemoryManager::m_all_cache_cntlrs;

MemoryManager::MemoryManager(Core* core,
      Network* network, ShmemPerfModel* shmem_perf_model):
   MemoryManagerBase(core, network, shmem_perf_model),
   m_dram_cache(NULL),
   m_dram_directory_cntlr(NULL),
   m_dram_cntlr(NULL),
   m_itlb(NULL), m_dtlb(NULL),
   m_tlb_miss_penalty(NULL,0),
   m_dram_cntlr_present(false),
   m_enabled(false)
{
   // Read Parameters from the Config file
   std::map<MemComponent::component_t, CacheParameters> cache_parameters;
   std::map<MemComponent::component_t, String> cache_names;

   bool dram_direct_access = false;
   UInt32 dram_directory_total_entries = 0;
   UInt32 dram_directory_associativity = 0;
   UInt32 dram_directory_max_num_sharers = 0;
   UInt32 dram_directory_max_hw_sharers = 0;
   String dram_directory_type_str;
   UInt32 dram_directory_home_lookup_param = 0;
   SubsecondTime dram_directory_cache_access_time = SubsecondTime::Zero();

   SubsecondTime dram_latency(SubsecondTime::Zero());
   ComponentBandwidth per_dram_controller_bandwidth(0);
   bool dram_queue_model_enabled = false;
   String dram_queue_model_type;

   try
   {
      m_cache_block_size = Sim()->getCfg()->getInt("perf_model/l1_icache/cache_block_size");

      m_last_level_cache = (MemComponent::component_t)(Sim()->getCfg()->getInt("perf_model/cache/levels") - 2 + MemComponent::L2_CACHE);

      UInt32 itlb_size = Sim()->getCfg()->getInt("perf_model/itlb/size");
      if (itlb_size)
         m_itlb = new TLB("itlb", getCore()->getId(), itlb_size, Sim()->getCfg()->getInt("perf_model/itlb/associativity"));
      UInt32 dtlb_size = Sim()->getCfg()->getInt("perf_model/dtlb/size");
      if (dtlb_size)
         m_dtlb = new TLB("dtlb", getCore()->getId(), dtlb_size, Sim()->getCfg()->getInt("perf_model/dtlb/associativity"));
      m_tlb_miss_penalty = ComponentLatency(core->getDvfsDomain(), Sim()->getCfg()->getInt("perf_model/tlb/penalty"));

      UInt32 smt_cores = Sim()->getCfg()->getInt("perf_model/core/logical_cpus");

      for(UInt32 i = MemComponent::FIRST_LEVEL_CACHE; i <= (UInt32)m_last_level_cache; ++i) {
         String configName, objectName;
         switch((MemComponent::component_t)i) {
            case MemComponent::L1_ICACHE:
               configName = "l1_icache";
               objectName = "L1-I";
               break;
            case MemComponent::L1_DCACHE:
               configName = "l1_dcache";
               objectName = "L1-D";
               break;
            default:
               String level = itostr(i - MemComponent::L2_CACHE + 2);
               configName = "l" + level + "_cache";
               objectName = "L" + level;
               break;
         }

         const ComponentPeriod *clock_domain = NULL;
         String domain_name = Sim()->getCfg()->getStringArray("perf_model/" + configName + "/dvfs_domain", core->getId());
         if (domain_name == "core")
            clock_domain = core->getDvfsDomain();
         else if (domain_name == "global")
            clock_domain = Sim()->getDvfsManager()->getGlobalDomain();
         else
            LOG_PRINT_ERROR("dvfs_domain %s is invalid", domain_name.c_str());

         cache_parameters[(MemComponent::component_t)i] = CacheParameters(
            configName,
            Sim()->getCfg()->getIntArray(   "perf_model/" + configName + "/cache_size", core->getId()),
            Sim()->getCfg()->getIntArray(   "perf_model/" + configName + "/associativity", core->getId()),
            Sim()->getCfg()->getStringArray("perf_model/" + configName + "/replacement_policy", core->getId()),
            Sim()->getCfg()->getBoolArray(  "perf_model/" + configName + "/perfect", core->getId()),
            i == MemComponent::L1_ICACHE
               ? Sim()->getCfg()->getBoolArray(  "perf_model/" + configName + "/coherent", core->getId())
               : true,
            ComponentLatency(clock_domain, Sim()->getCfg()->getIntArray("perf_model/" + configName + "/data_access_time", core->getId())),
            ComponentLatency(clock_domain, Sim()->getCfg()->getIntArray("perf_model/" + configName + "/tags_access_time", core->getId())),
            ComponentLatency(clock_domain, Sim()->getCfg()->getIntArray("perf_model/" + configName + "/writeback_time", core->getId())),
            ComponentBandwidthPerCycle(clock_domain,
               i < (UInt32)m_last_level_cache
                  ? Sim()->getCfg()->getIntArray("perf_model/" + configName + "/next_level_read_bandwidth", core->getId())
                  : 0),
            Sim()->getCfg()->getStringArray("perf_model/" + configName + "/perf_model_type", core->getId()),
            Sim()->getCfg()->getBoolArray(  "perf_model/" + configName + "/writethrough", core->getId()),
            Sim()->getCfg()->getIntArray(   "perf_model/" + configName + "/shared_cores", core->getId()) * smt_cores,
            i >= MemComponent::L2_CACHE
               ? Sim()->getCfg()->getStringArray("perf_model/" + configName + "/prefetcher", core->getId())
               : "none",
            i == MemComponent::L1_DCACHE
               ? Sim()->getCfg()->getIntArray(   "perf_model/" + configName + "/outstanding_misses", core->getId())
               : 0
         );
         cache_names[(MemComponent::component_t)i] = objectName;

         /* Non-application threads will be distributed at 1 per process, probably not as shared_cores per process.
            Still, they need caches (for inter-process data communication, not for modeling target timing).
            Make them non-shared so we don't create process-spanning shared caches. */
         if (getCore()->getId() >= (core_id_t) Sim()->getConfig()->getApplicationCores())
            cache_parameters[(MemComponent::component_t)i].shared_cores = 1;
      }

      // Dram Directory Cache
      dram_directory_total_entries = Sim()->getCfg()->getInt("perf_model/dram_directory/total_entries");
      dram_directory_associativity = Sim()->getCfg()->getInt("perf_model/dram_directory/associativity");
      dram_directory_max_num_sharers = Sim()->getConfig()->getTotalCores();
      dram_directory_max_hw_sharers = Sim()->getCfg()->getInt("perf_model/dram_directory/max_hw_sharers");
      dram_directory_type_str = Sim()->getCfg()->getString("perf_model/dram_directory/directory_type");
      dram_directory_home_lookup_param = Sim()->getCfg()->getInt("perf_model/dram_directory/home_lookup_param");
      dram_directory_cache_access_time = SubsecondTime::NS() * Sim()->getCfg()->getInt("perf_model/dram_directory/directory_cache_access_time");

      // Dram Cntlr
      dram_direct_access = Sim()->getCfg()->getBool("perf_model/dram/direct_access");
      dram_latency = SubsecondTime::FS() * static_cast<uint64_t>(TimeConverter<float>::NStoFS(Sim()->getCfg()->getFloat("perf_model/dram/latency"))); // Operate in fs for higher precision before converting to uint64_t/SubsecondTime
      per_dram_controller_bandwidth = ComponentBandwidth(8 * Sim()->getCfg()->getFloat("perf_model/dram/per_controller_bandwidth")); // Convert bytes to bits
      dram_queue_model_enabled = Sim()->getCfg()->getBool("perf_model/dram/queue_model/enabled");
      dram_queue_model_type = Sim()->getCfg()->getString("perf_model/dram/queue_model/type");
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

      m_dram_cntlr = new PrL1PrL2DramDirectoryMSI::DramCntlr(this,
            dram_latency,
            per_dram_controller_bandwidth,
            dram_queue_model_enabled,
            dram_queue_model_type,
            getCacheBlockSize());

      if (Sim()->getCfg()->getBoolArray("perf_model/dram/cache/enabled", core->getId()))
      {
         m_dram_cache = new DramCache(core->getId(), getCacheBlockSize(), m_dram_cntlr);
      }

      m_dram_directory_cntlr = new PrL1PrL2DramDirectoryMSI::DramDirectoryCntlr(getCore()->getId(),
            this,
            m_dram_cache ? (DramCntlrInterface*)m_dram_cache : (DramCntlrInterface*)m_dram_cntlr,
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

   for(UInt32 i = MemComponent::FIRST_LEVEL_CACHE; i <= (UInt32)m_last_level_cache; ++i) {
      CacheCntlr* cache_cntlr = new CacheCntlr(
         (MemComponent::component_t)i,
         cache_names[(MemComponent::component_t)i],
         getCore()->getId(),
         this,
         m_dram_directory_home_lookup,
         m_user_thread_sem,
         m_network_thread_sem,
         getCacheBlockSize(),
         cache_parameters[(MemComponent::component_t)i],
         getShmemPerfModel()
      );
      m_cache_cntlrs[(MemComponent::component_t)i] = cache_cntlr;
      setCacheCntlrAt(getCore()->getId(), (MemComponent::component_t)i, cache_cntlr);
   }

   m_cache_cntlrs[MemComponent::L1_ICACHE]->setNextCacheCntlr(m_cache_cntlrs[MemComponent::L2_CACHE]);
   m_cache_cntlrs[MemComponent::L1_DCACHE]->setNextCacheCntlr(m_cache_cntlrs[MemComponent::L2_CACHE]);
   for(UInt32 i = MemComponent::L2_CACHE; i <= (UInt32)m_last_level_cache - 1; ++i)
      m_cache_cntlrs[(MemComponent::component_t)i]->setNextCacheCntlr(m_cache_cntlrs[(MemComponent::component_t)(i + 1)]);

   CacheCntlrList prev_cache_cntlrs;
   prev_cache_cntlrs.push_back(m_cache_cntlrs[MemComponent::L1_ICACHE]);
   prev_cache_cntlrs.push_back(m_cache_cntlrs[MemComponent::L1_DCACHE]);
   m_cache_cntlrs[MemComponent::L2_CACHE]->setPrevCacheCntlrs(prev_cache_cntlrs);

   for(UInt32 i = MemComponent::L2_CACHE; i <= (UInt32)m_last_level_cache - 1; ++i) {
      CacheCntlrList prev_cache_cntlrs;
      prev_cache_cntlrs.push_back(m_cache_cntlrs[(MemComponent::component_t)i]);
      m_cache_cntlrs[(MemComponent::component_t)(i + 1)]->setPrevCacheCntlrs(prev_cache_cntlrs);
   }

   // Create Performance Models
   for(UInt32 i = MemComponent::FIRST_LEVEL_CACHE; i <= (UInt32)m_last_level_cache; ++i)
      m_cache_perf_models[(MemComponent::component_t)i] = CachePerfModel::create(
       cache_parameters[(MemComponent::component_t)i].perf_model_type,
       cache_parameters[(MemComponent::component_t)i].data_access_time,
       cache_parameters[(MemComponent::component_t)i].tags_access_time
      );


   if (m_dram_cntlr_present)
      LOG_ASSERT_ERROR(m_cache_cntlrs[m_last_level_cache]->isMasterCache() == true, "DRAM controllers may only be at 'master' node of shared caches");


   // The core id to use when sending messages to the directory (master node of the last-level cache)
   m_core_id_master = getCore()->getId() - getCore()->getId() % cache_parameters[m_last_level_cache].shared_cores;

   if (m_core_id_master == getCore()->getId()) {
      m_cache_cntlrs[(UInt32)m_last_level_cache]->createSetLocks(
         getCacheBlockSize(),
         k_KILO * cache_parameters[MemComponent::L1_DCACHE].size / (cache_parameters[MemComponent::L1_DCACHE].associativity * getCacheBlockSize()),
         m_core_id_master,
         cache_parameters[m_last_level_cache].shared_cores
      );
      if (dram_direct_access && getCore()->getId() < (core_id_t)Sim()->getConfig()->getApplicationCores())
      {
         LOG_ASSERT_ERROR(Sim()->getConfig()->getApplicationCores() <= cache_parameters[m_last_level_cache].shared_cores, "DRAM direct access is only possible when there is just a single last-level cache (LLC level %d shared by %d, num cores %d)", m_last_level_cache, cache_parameters[m_last_level_cache].shared_cores, Sim()->getConfig()->getApplicationCores());
         LOG_ASSERT_ERROR(m_dram_cntlr != NULL, "I'm supposed to have direct access to a DRAM controller, but there isn't one at this node");
         m_cache_cntlrs[(UInt32)m_last_level_cache]->setDRAMDirectAccess(m_dram_cache ? (DramCntlrInterface*)m_dram_cache : (DramCntlrInterface*)m_dram_cntlr);
      }
   }

   // Register Call-backs
   getNetwork()->registerCallback(SHARED_MEM_1, MemoryManagerNetworkCallback, this);
   getNetwork()->registerCallback(SHARED_MEM_2, MemoryManagerNetworkCallback, this);
}

MemoryManager::~MemoryManager()
{
   UInt32 i;

   getNetwork()->unregisterCallback(SHARED_MEM_1);
   getNetwork()->unregisterCallback(SHARED_MEM_2);

   // Delete the Models

   if (m_itlb) delete m_itlb;
   if (m_dtlb) delete m_dtlb;

   for(i = MemComponent::FIRST_LEVEL_CACHE; i <= (UInt32)m_last_level_cache; ++i)
   {
      delete m_cache_perf_models[(MemComponent::component_t)i];
      m_cache_perf_models[(MemComponent::component_t)i] = NULL;
   }

   delete m_user_thread_sem;
   delete m_network_thread_sem;
   delete m_dram_directory_home_lookup;

   for(i = MemComponent::FIRST_LEVEL_CACHE; i <= (UInt32)m_last_level_cache; ++i)
   {
      delete m_cache_cntlrs[(MemComponent::component_t)i];
      m_cache_cntlrs[(MemComponent::component_t)i] = NULL;
   }

   if (m_dram_cache)
      delete m_dram_cache;

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
   LOG_ASSERT_ERROR(mem_component <= m_last_level_cache,
      "Error: invalid mem_component (%d) for coreInitiateMemoryAccess", mem_component);

   if (mem_component == MemComponent::L1_ICACHE && m_itlb)
      accessTLB(m_itlb, address, true, modeled);
   else if (mem_component == MemComponent::L1_DCACHE && m_dtlb)
      accessTLB(m_dtlb, address, false, modeled);

   return m_cache_cntlrs[mem_component]->processMemOpFromCore(
         lock_signal,
         mem_op_type,
         address, offset,
         data_buf, data_length,
         modeled == Core::MEM_MODELED_NONE ? false : true);
}

void
MemoryManager::handleMsgFromNetwork(NetPacket& packet)
{
MYLOG("begin");
   core_id_t sender = packet.sender;
   PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg = PrL1PrL2DramDirectoryMSI::ShmemMsg::getShmemMsg((Byte*) packet.data);
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
      case MemComponent::L2_CACHE: /* PrL1PrL2DramDirectoryMSI::DramCntlr sends to L2 and doesn't know about our other levels */
      case MemComponent::LAST_LEVEL_CACHE:
         switch(sender_mem_component)
         {
            case MemComponent::DRAM_DIR:
               m_cache_cntlrs[m_last_level_cache]->handleMsgFromDramDirectory(sender, shmem_msg);
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

            case MemComponent::LAST_LEVEL_CACHE:
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
MYLOG("end");
}

void
MemoryManager::sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::msg_t msg_type, MemComponent::component_t sender_mem_component, MemComponent::component_t receiver_mem_component, core_id_t requester, core_id_t receiver, IntPtr address, Byte* data_buf, UInt32 data_length, HitWhere::where_t where, ShmemPerfModel::Thread_t thread_num)
{
MYLOG("send msg %u %ul%u > %ul%u", msg_type, requester, sender_mem_component, receiver, receiver_mem_component);
   assert((data_buf == NULL) == (data_length == 0));
   PrL1PrL2DramDirectoryMSI::ShmemMsg shmem_msg(msg_type, sender_mem_component, receiver_mem_component, requester, address, data_buf, data_length);
   shmem_msg.setWhere(where);

   Byte* msg_buf = shmem_msg.makeMsgBuf();
   SubsecondTime msg_time = getShmemPerfModel()->getElapsedTime(thread_num);

   if (m_enabled)
   {
      LOG_PRINT("Sending Msg: type(%u), address(0x%x), sender_mem_component(%u), receiver_mem_component(%u), requester(%i), sender(%i), receiver(%i)", msg_type, address, sender_mem_component, receiver_mem_component, requester, getCore()->getId(), receiver);
   }

   NetPacket packet(msg_time, SHARED_MEM_1,
         m_core_id_master, receiver,
         shmem_msg.getMsgLen(), (const void*) msg_buf);
   getNetwork()->netSend(packet);

   // Delete the Msg Buf
   delete [] msg_buf;
}

void
MemoryManager::broadcastMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::msg_t msg_type, MemComponent::component_t sender_mem_component, MemComponent::component_t receiver_mem_component, core_id_t requester, IntPtr address, Byte* data_buf, UInt32 data_length, ShmemPerfModel::Thread_t thread_num)
{
MYLOG("bcast msg");
   assert((data_buf == NULL) == (data_length == 0));
   PrL1PrL2DramDirectoryMSI::ShmemMsg shmem_msg(msg_type, sender_mem_component, receiver_mem_component, requester, address, data_buf, data_length);

   Byte* msg_buf = shmem_msg.makeMsgBuf();
   SubsecondTime msg_time = getShmemPerfModel()->getElapsedTime(thread_num);

   if (m_enabled)
   {
      LOG_PRINT("Sending Msg: type(%u), address(0x%x), sender_mem_component(%u), receiver_mem_component(%u), requester(%i), sender(%i), receiver(%i)", msg_type, address, sender_mem_component, receiver_mem_component, requester, getCore()->getId(), NetPacket::BROADCAST);
   }

   NetPacket packet(msg_time, SHARED_MEM_1,
         m_core_id_master, NetPacket::BROADCAST,
         shmem_msg.getMsgLen(), (const void*) msg_buf);
   getNetwork()->netSend(packet);

   // Delete the Msg Buf
   delete [] msg_buf;
}

void
MemoryManager::accessTLB(TLB * tlb, IntPtr address, bool isIfetch, Core::MemModeled modeled)
{
   bool hit = tlb->lookup(address, getShmemPerfModel()->getElapsedTime());
   if (!hit && !(modeled == Core::MEM_MODELED_NONE || modeled == Core::MEM_MODELED_COUNT)) {
      Instruction *i = new TLBMissInstruction(m_tlb_miss_penalty.getLatency(), isIfetch);
      getCore()->getPerformanceModel()->queueDynamicInstruction(i);
   }
}

SubsecondTime
MemoryManager::getCost(MemComponent::component_t mem_component, CachePerfModel::CacheAccess_t access_type)
{
   if (mem_component == MemComponent::INVALID_MEM_COMPONENT)
      return SubsecondTime::Zero();

   return m_cache_perf_models[mem_component]->getLatency(access_type);
}

void
MemoryManager::incrElapsedTime(SubsecondTime latency, ShmemPerfModel::Thread_t thread_num)
{
   MYLOG("cycles += %s", itostr(latency).c_str());
   getShmemPerfModel()->incrElapsedTime(latency, thread_num);
}

void
MemoryManager::incrElapsedTime(MemComponent::component_t mem_component, CachePerfModel::CacheAccess_t access_type, ShmemPerfModel::Thread_t thread_num)
{
   incrElapsedTime(getCost(mem_component, access_type), thread_num);
}

void
MemoryManager::enableModels()
{
   m_enabled = true;

   for(UInt32 i = MemComponent::FIRST_LEVEL_CACHE; i <= (UInt32)m_last_level_cache; ++i)
   {
      m_cache_cntlrs[(MemComponent::component_t)i]->enable();
      m_cache_perf_models[(MemComponent::component_t)i]->enable();
   }

   if (m_dram_cntlr_present)
      m_dram_cntlr->getDramPerfModel()->enable();
}

void
MemoryManager::disableModels()
{
   m_enabled = false;

   for(UInt32 i = MemComponent::FIRST_LEVEL_CACHE; i <= (UInt32)m_last_level_cache; ++i)
   {
      m_cache_cntlrs[(MemComponent::component_t)i]->disable();
      m_cache_perf_models[(MemComponent::component_t)i]->disable();
   }

   if (m_dram_cntlr_present)
      m_dram_cntlr->getDramPerfModel()->disable();
}

void
MemoryManager::outputSummary(std::ostream &os)
{
   os << "Cache Summary:\n";
   for(UInt32 i = MemComponent::FIRST_LEVEL_CACHE; i <= (UInt32)m_last_level_cache; ++i)
   {
      m_cache_cntlrs[(MemComponent::component_t)i]->outputSummary(os);
   }

   if (m_dram_cntlr_present)
      m_dram_cntlr->getDramPerfModel()->outputSummary(os);
   else
      DramPerfModel::dummyOutputSummary(os);
}

}
