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
      m_data_length(0)
   {}

   ShmemMsg::ShmemMsg(msg_t msg_type,
         MemComponent::component_t sender_mem_component,
         MemComponent::component_t receiver_mem_component,
         core_id_t requester,
         IntPtr address,
         Byte* data_buf,
         UInt32 data_length) :
      m_msg_type(msg_type),
      m_sender_mem_component(sender_mem_component),
      m_receiver_mem_component(receiver_mem_component),
      m_requester(requester),
      m_where(HitWhere::UNKNOWN),
      m_address(address),
      m_data_buf(data_buf),
      m_data_length(data_length)
   {}

   ShmemMsg::ShmemMsg(ShmemMsg* shmem_msg) :
      m_msg_type(shmem_msg->getMsgType()),
      m_sender_mem_component(shmem_msg->getSenderMemComponent()),
      m_receiver_mem_component(shmem_msg->getReceiverMemComponent()),
      m_requester(shmem_msg->getRequester()),
      m_address(shmem_msg->getAddress()),
      m_data_buf(shmem_msg->getDataBuf()),
      m_data_length(shmem_msg->getDataLength())
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

}
