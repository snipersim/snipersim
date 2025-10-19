#include <string.h>
#include "shmem_msg.h"
#include "shmem_perf.h"
#include "log.h"

namespace PrL1PrL2DramDirectoryMSI
{
   ShmemMsg::ShmemMsg(ShmemPerf *perf) : m_msg_type(INVALID_MSG_TYPE),
                                         m_sender_mem_component(MemComponent::INVALID_MEM_COMPONENT),
                                         m_receiver_mem_component(MemComponent::INVALID_MEM_COMPONENT),
                                         m_requester(INVALID_CORE_ID),
                                         m_where(HitWhere::UNKNOWN),
                                         m_address(INVALID_ADDRESS),
                                         m_data_buf(NULL),
                                         m_data_length(0),
                                         m_perf(perf),
                                         m_block_type(CacheBlockInfo::block_type_t::NON_PAGE_TABLE)
   {
   }

   ShmemMsg::ShmemMsg(msg_t msg_type,
                      MemComponent::component_t sender_mem_component,
                      MemComponent::component_t receiver_mem_component,
                      core_id_t requester,
                      IntPtr address,
                      Byte *data_buf,
                      UInt32 data_length,
                      ShmemPerf *perf, CacheBlockInfo::block_type_t block_type) : m_msg_type(msg_type),
                                                                                  m_sender_mem_component(sender_mem_component),
                                                                                  m_receiver_mem_component(receiver_mem_component),
                                                                                  m_requester(requester),
                                                                                  m_where(HitWhere::UNKNOWN),
                                                                                  m_address(address),
                                                                                  m_data_buf(data_buf),
                                                                                  m_data_length(data_length),
                                                                                  m_perf(perf),
                                                                                  m_block_type(block_type)
   {
   }

   ShmemMsg::ShmemMsg(ShmemMsg *shmem_msg) : m_msg_type(shmem_msg->getMsgType()),
                                             m_sender_mem_component(shmem_msg->getSenderMemComponent()),
                                             m_receiver_mem_component(shmem_msg->getReceiverMemComponent()),
                                             m_requester(shmem_msg->getRequester()),
                                             m_address(shmem_msg->getAddress()),
                                             m_data_buf(shmem_msg->getDataBuf()),
                                             m_data_length(shmem_msg->getDataLength()),
                                             m_perf(shmem_msg->getPerf()),
                                             m_block_type(shmem_msg->getBlockType())
   {
   }

   ShmemMsg::~ShmemMsg()
   {
   }

   ShmemMsg *ShmemMsg::getShmemMsg(Byte *msg_buf, ShmemPerf *perf)
   {
      // Create a temporary ShmemMsg to read values from msg_buf without triggering its constructor's default initializations
      // or to directly map the incoming bytes to a struct-like representation.
      // A safer approach is to read each field explicitly.

      // We'll use a temporary structure that matches the layout for reading from msg_buf
      // IMPORTANT: This assumes the sender and receiver have the *exact same* memory layout for ShmemMsg.
      // This is generally brittle. A more robust solution would be to serialize/deserialize each member individually.
      struct ShmemMsgRaw
      {
         ShmemMsg::msg_t m_msg_type;
         MemComponent::component_t m_sender_mem_component;
         MemComponent::component_t m_receiver_mem_component;
         core_id_t m_requester;
         HitWhere::where_t m_where;
         IntPtr m_address;
         Byte *m_data_buf; // This pointer will be invalid after memcpy
         UInt32 m_data_length;
         ShmemPerf *m_perf; // This pointer will be invalid after memcpy
         CacheBlockInfo::block_type_t m_block_type;
      };

      ShmemMsgRaw raw_msg;
      memcpy(&raw_msg, msg_buf, sizeof(ShmemMsgRaw));

      // Now, use the values from raw_msg to construct the actual ShmemMsg object
      ShmemMsg *shmem_msg = new ShmemMsg(
          raw_msg.m_msg_type,
          raw_msg.m_sender_mem_component,
          raw_msg.m_receiver_mem_component,
          raw_msg.m_requester,
          raw_msg.m_address,
          nullptr, // data_buf will be handled separately
          raw_msg.m_data_length,
          perf, // Use the passed-in perf, not the one from raw_msg
          raw_msg.m_block_type);

      // Handle data_buf separately, as it's a pointer and needs new allocation
      if (shmem_msg->getDataLength() > 0)
      {
         shmem_msg->setDataBuf(new Byte[shmem_msg->getDataLength()]);
         memcpy((void *)shmem_msg->getDataBuf(), msg_buf + sizeof(ShmemMsgRaw), shmem_msg->getDataLength());
      }
      // Set the 'where' since it's not in the constructor
      shmem_msg->setWhere(raw_msg.m_where);

      return shmem_msg;
   }

   Byte *
   ShmemMsg::makeMsgBuf()
   {
      Byte *msg_buf = new Byte[getMsgLen()];
      memcpy(msg_buf, (void *)this, sizeof(*this));
      if (m_data_length > 0)
      {
         LOG_ASSERT_ERROR(m_data_buf != NULL, "m_data_buf(%p)", m_data_buf);
         memcpy(msg_buf + sizeof(*this), (void *)m_data_buf, m_data_length);
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
      switch (m_msg_type)
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
