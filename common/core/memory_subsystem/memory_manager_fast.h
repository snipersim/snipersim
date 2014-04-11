#ifndef __MEMORY_MANAGER_FAST_H
#define __MEMORY_MANAGER_FAST_H

#include "memory_manager_base.h"
#include "mem_component.h"
#include "fixed_types.h"
#include "subsecond_time.h"

class MemoryManagerFast : public MemoryManagerBase
{
   protected:
      static const UInt64 CACHE_LINE_BITS = 6;
      static const UInt64 CACHE_LINE_SIZE = 1 << CACHE_LINE_BITS;

   public:
      MemoryManagerFast(Core* core, Network* network, ShmemPerfModel* shmem_perf_model)
         : MemoryManagerBase(core, network, shmem_perf_model)
      {}
      virtual ~MemoryManagerFast() {}

      HitWhere::where_t coreInitiateMemoryAccess(
            MemComponent::component_t mem_component,
            Core::lock_signal_t lock_signal,
            Core::mem_op_t mem_op_type,
            IntPtr address, UInt32 offset,
            Byte* data_buf, UInt32 data_length,
            Core::MemModeled modeled)
      {
         // Emulate slow interface by calling into fast interface
         assert(data_buf == NULL);
         SubsecondTime latency = coreInitiateMemoryAccessFast(mem_component == MemComponent::L1_ICACHE ? true : false, mem_op_type, address);
         getShmemPerfModel()->incrElapsedTime(latency,  ShmemPerfModel::_USER_THREAD);
         if (latency > SubsecondTime::Zero())
            return HitWhere::MISS;
         else
            return HitWhere::where_t(mem_component);
      }

      virtual SubsecondTime coreInitiateMemoryAccessFast(
            bool icache,
            Core::mem_op_t mem_op_type,
            IntPtr address) = 0;

      void handleMsgFromNetwork(NetPacket& packet) { assert(false); }

      void enableModels() {}
      void disableModels() {}

      core_id_t getShmemRequester(const void* pkt_data) { assert(false); }
      UInt32 getModeledLength(const void* pkt_data) { assert(false); }

      #ifndef OPT_CACHEBLOCKSIZE
      UInt64 getCacheBlockSize() const { return CACHE_LINE_SIZE; }
      #endif

      void sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::msg_t msg_type, MemComponent::component_t sender_mem_component, MemComponent::component_t receiver_mem_component, core_id_t requester, core_id_t receiver, IntPtr address, Byte* data_buf = NULL, UInt32 data_length = 0, HitWhere::where_t where = HitWhere::UNKNOWN, ShmemPerf *perf = NULL, ShmemPerfModel::Thread_t thread_num = ShmemPerfModel::NUM_CORE_THREADS) { assert(false); }
      void broadcastMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::msg_t msg_type, MemComponent::component_t sender_mem_component, MemComponent::component_t receiver_mem_component, core_id_t requester, IntPtr address, Byte* data_buf = NULL, UInt32 data_length = 0, ShmemPerf *perf = NULL, ShmemPerfModel::Thread_t thread_num = ShmemPerfModel::NUM_CORE_THREADS) { assert(false); }

      SubsecondTime getL1HitLatency(void) { return SubsecondTime::Zero(); }
      void addL1Hits(bool icache, Core::mem_op_t mem_op_type, UInt64 hits) {}
};

#endif // __MEMORY_MANAGER_FAST_H
