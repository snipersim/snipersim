#pragma once

#include "shmem_msg.h"
#include "fixed_types.h"
#include "subsecond_time.h"

namespace PrL1PrL2DramDirectoryMSI
{
   class ShmemReq
   {
      private:
         ShmemMsg* m_shmem_msg;
         SubsecondTime m_time;
         bool m_wait_for_data;
         core_id_t m_forwarding_from;

      public:
         ShmemReq(ShmemMsg* shmem_msg, SubsecondTime time);
         ~ShmemReq();

         ShmemMsg* getShmemMsg() const { return m_shmem_msg; }
         SubsecondTime getTime() const { return m_time; }
         bool getWaitForData() const { return m_wait_for_data; }
         core_id_t getForwardingFrom() const { return m_forwarding_from; }
         bool isForwarding() const { return m_forwarding_from != INVALID_CORE_ID; }

         void setTime(SubsecondTime time) { m_time = time; }
         void updateTime(SubsecondTime time)
         {
            if (time > m_time)
               m_time = time;
         }
         void setWaitForData(bool wait) { m_wait_for_data = wait; }
         void setForwardingFrom(core_id_t core_id) { m_forwarding_from = core_id; }
   };

}
