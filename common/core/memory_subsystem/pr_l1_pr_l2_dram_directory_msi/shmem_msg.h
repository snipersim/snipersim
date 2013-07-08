#pragma once

#include "mem_component.h"
#include "fixed_types.h"
#include "dynamic_instruction_info.h"

class ShmemPerf;

namespace PrL1PrL2DramDirectoryMSI
{
   class ShmemMsg
   {
      public:
         enum msg_t
         {
            INVALID_MSG_TYPE = 0,
            MIN_MSG_TYPE,
            // Cache > tag directory
            EX_REQ = MIN_MSG_TYPE,
            SH_REQ,
            UPGRADE_REQ,
            INV_REQ,
            FLUSH_REQ,
            WB_REQ,
            // Tag directory > cache
            EX_REP,
            SH_REP,
            UPGRADE_REP,
            INV_REP,
            FLUSH_REP,
            WB_REP,
            NULLIFY_REQ,
            // Tag directory > DRAM
            DRAM_READ_REQ,
            DRAM_WRITE_REQ,
            // DRAM > tag directory
            DRAM_READ_REP,

            MAX_MSG_TYPE = NULLIFY_REQ,
            NUM_MSG_TYPES = MAX_MSG_TYPE - MIN_MSG_TYPE + 1
         };

      private:
         msg_t m_msg_type;
         MemComponent::component_t m_sender_mem_component;
         MemComponent::component_t m_receiver_mem_component;
         core_id_t m_requester;
         HitWhere::where_t m_where;
         IntPtr m_address;
         Byte* m_data_buf;
         UInt32 m_data_length;
         ShmemPerf* m_perf;

      public:
         ShmemMsg();
         ShmemMsg(msg_t msg_type,
               MemComponent::component_t sender_mem_component,
               MemComponent::component_t receiver_mem_component,
               core_id_t requester,
               IntPtr address,
               Byte* data_buf,
               UInt32 data_length,
               ShmemPerf* perf);
         ShmemMsg(ShmemMsg* shmem_msg);

         ~ShmemMsg();

         static ShmemMsg* getShmemMsg(Byte* msg_buf);
         Byte* makeMsgBuf();
         UInt32 getMsgLen();

         // Modeling
         UInt32 getModeledLength();

         msg_t getMsgType() { return m_msg_type; }
         MemComponent::component_t getSenderMemComponent() { return m_sender_mem_component; }
         MemComponent::component_t getReceiverMemComponent() { return m_receiver_mem_component; }
         core_id_t getRequester() { return m_requester; }
         IntPtr getAddress() { return m_address; }
         Byte* getDataBuf() { return m_data_buf; }
         UInt32 getDataLength() { return m_data_length; }
         HitWhere::where_t getWhere() { return m_where; }

         void setDataBuf(Byte* data_buf) { m_data_buf = data_buf; }
         void setWhere(HitWhere::where_t where) { m_where = where; }

         ShmemPerf* getPerf() { return m_perf; }

   };

}
