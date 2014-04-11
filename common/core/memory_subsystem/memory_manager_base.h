#ifndef __MEMORY_MANAGER_BASE_H__
#define __MEMORY_MANAGER_BASE_H__

#include "core.h"
#include "network.h"
#include "mem_component.h"
#include "performance_model.h"
#include "shmem_perf_model.h"
#include "pr_l1_pr_l2_dram_directory_msi/shmem_msg.h"

void MemoryManagerNetworkCallback(void* obj, NetPacket packet);

class MemoryManagerBase
{
   public:
      enum CachingProtocol_t
      {
         PARAMETRIC_DRAM_DIRECTORY_MSI,
         FAST_NEHALEM,
         NUM_CACHING_PROTOCOL_TYPES
      };

   private:
      Core* m_core;
      Network* m_network;
      ShmemPerfModel* m_shmem_perf_model;

      void parseMemoryControllerList(String& memory_controller_positions, std::vector<core_id_t>& core_list_from_cfg_file, SInt32 application_core_count);

   protected:
      Network* getNetwork() { return m_network; }
      ShmemPerfModel* getShmemPerfModel() { return m_shmem_perf_model; }

      std::vector<core_id_t> getCoreListWithMemoryControllers(void);
      void printCoreListWithMemoryControllers(std::vector<core_id_t>& core_list_with_memory_controllers);

   public:
      MemoryManagerBase(Core* core, Network* network, ShmemPerfModel* shmem_perf_model):
         m_core(core),
         m_network(network),
         m_shmem_perf_model(shmem_perf_model)
      {}
      virtual ~MemoryManagerBase() {}

      virtual HitWhere::where_t coreInitiateMemoryAccess(
            MemComponent::component_t mem_component,
            Core::lock_signal_t lock_signal,
            Core::mem_op_t mem_op_type,
            IntPtr address, UInt32 offset,
            Byte* data_buf, UInt32 data_length,
            Core::MemModeled modeled) = 0;
      virtual SubsecondTime coreInitiateMemoryAccessFast(
            bool icache,
            Core::mem_op_t mem_op_type,
            IntPtr address)
      {
         // Emulate fast interface by calling into slow interface
         SubsecondTime initial_time = getCore()->getPerformanceModel()->getElapsedTime();
         getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, initial_time);

         coreInitiateMemoryAccess(
               icache ? MemComponent::L1_ICACHE : MemComponent::L1_DCACHE,
               Core::NONE,
               mem_op_type,
               address - (address % getCacheBlockSize()), 0,
               NULL, getCacheBlockSize(),
               Core::MEM_MODELED_COUNT_TLBTIME);

         // Get the final cycle time
         SubsecondTime final_time = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
         SubsecondTime latency = final_time - initial_time;
         return latency;
      }

      virtual void handleMsgFromNetwork(NetPacket& packet) = 0;

      // FIXME: Take this out of here
      virtual UInt64 getCacheBlockSize() const = 0;

      virtual SubsecondTime getL1HitLatency(void) = 0;
      virtual void addL1Hits(bool icache, Core::mem_op_t mem_op_type, UInt64 hits) = 0;

      virtual core_id_t getShmemRequester(const void* pkt_data) = 0;

      virtual void enableModels() = 0;
      virtual void disableModels() = 0;

      // Modeling
      virtual UInt32 getModeledLength(const void* pkt_data) = 0;

      Core* getCore() { return m_core; }

      virtual void sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::msg_t msg_type, MemComponent::component_t sender_mem_component, MemComponent::component_t receiver_mem_component, core_id_t requester, core_id_t receiver, IntPtr address, Byte* data_buf = NULL, UInt32 data_length = 0, HitWhere::where_t where = HitWhere::UNKNOWN, ShmemPerf *perf = NULL, ShmemPerfModel::Thread_t thread_num = ShmemPerfModel::NUM_CORE_THREADS) = 0;
      virtual void broadcastMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::msg_t msg_type, MemComponent::component_t sender_mem_component, MemComponent::component_t receiver_mem_component, core_id_t requester, IntPtr address, Byte* data_buf = NULL, UInt32 data_length = 0, ShmemPerf *perf = NULL, ShmemPerfModel::Thread_t thread_num = ShmemPerfModel::NUM_CORE_THREADS) = 0;

      static CachingProtocol_t parseProtocolType(String& protocol_type);
      static MemoryManagerBase* createMMU(String protocol_type,
            Core* core,
            Network* network,
            ShmemPerfModel* shmem_perf_model);
};

#endif /* __MEMORY_MANAGER_BASE_H__ */
