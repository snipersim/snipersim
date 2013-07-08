#include <string.h>
#include "shmem_msg.h"
#include "log.h"

namespace PrL1PrL2DramDirectoryMSI
{
   ShmemMsg::ShmemMsg() :
      m_msg_type(INVALID_MSG_TYPE),
      m_sender_mem_component(MemComponent::INVALID_MEM_COMPONENT),
      m_receiver_mem_component(MemComponent::INVALID_MEM_COMPONENT),
      m_requester(INVALID_CORE_ID),
      m_where(HitWhere::UNKNOWN),
      m_address(INVALID_ADDRESS),
      m_data_buf(NULL),
      m_data_length(0),
      m_perf(NULL)
   {}

   ShmemMsg::ShmemMsg(msg_t msg_type,
         MemComponent::component_t sender_mem_component,
         MemComponent::component_t receiver_mem_component,
         core_id_t requester,
         IntPtr address,
         Byte* data_buf,
         UInt32 data_length,
         ShmemPerf* perf) :
      m_msg_type(msg_type),
      m_sender_mem_component(sender_mem_component),
      m_receiver_mem_component(receiver_mem_component),
      m_requester(requester),
      m_where(HitWhere::UNKNOWN),
      m_address(address),
      m_data_buf(data_buf),
      m_data_length(data_length),
      m_perf(perf)
   {}

   ShmemMsg::ShmemMsg(ShmemMsg* shmem_msg) :
      m_msg_type(shmem_msg->getMsgType()),
      m_sender_mem_component(shmem_msg->getSenderMemComponent()),
      m_receiver_mem_component(shmem_msg->getReceiverMemComponent()),
      m_requester(shmem_msg->getRequester()),
      m_address(shmem_msg->getAddress()),
      m_data_buf(shmem_msg->getDataBuf()),
      m_data_length(shmem_msg->getDataLength()),
      m_perf(shmem_msg->getPerf())
   {}

   ShmemMsg::~ShmemMsg()
   {}

   ShmemMsg*
   ShmemMsg::getShmemMsg(Byte* msg_buf)
   {
      ShmemMsg* shmem_msg = new ShmemMsg();
      memcpy((void*) shmem_msg, msg_buf, sizeof(*shmem_msg));
      if (shmem_msg->getDataLength() > 0)
      {
         shmem_msg->setDataBuf(new Byte[shmem_msg->getDataLength()]);
         memcpy((void*) shmem_msg->getDataBuf(), msg_buf + sizeof(*shmem_msg), shmem_msg->getDataLength());
      }
      return shmem_msg;
   }

   Byte*
   ShmemMsg::makeMsgBuf()
   {
      Byte* msg_buf = new Byte[getMsgLen()];
      memcpy(msg_buf, (void*) this, sizeof(*this));
      if (m_data_length > 0)
      {
         LOG_ASSERT_ERROR(m_data_buf != NULL, "m_data_buf(%p)", m_data_buf);
         memcpy(msg_buf + sizeof(*this), (void*) m_data_buf, m_data_length);
      }

      return msg_buf;
   }

   UInt32
   ShmemMsg::getMsgLen()
   {
      return (sizeof(*this) + m_data_length);
   }

   UInt32
   ShmemMsg::getModeledLength()
   {
      switch(m_msg_type)
      {
         case EX_REQ:
         case SH_REQ:
         case INV_REQ:
         case FLUSH_REQ:
         case WB_REQ:
         case UPGRADE_REP:
         case UPGRADE_REQ:
         case INV_REP:
         case DRAM_READ_REQ:
            // msg_type + address
            // msg_type - 1 byte
            return (1 + sizeof(IntPtr));

         case EX_REP:
         case SH_REP:
         case FLUSH_REP:
         case WB_REP:
         case DRAM_WRITE_REQ:
         case DRAM_READ_REP:
            // msg_type + address + cache_block
            return (1 + sizeof(IntPtr) + m_data_length);

         default:
            LOG_PRINT_ERROR("Unrecognized Msg Type(%u)", m_msg_type);
            return 0;
      }
   }

}
