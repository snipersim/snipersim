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

      public:
         ShmemReq(ShmemMsg* shmem_msg, SubsecondTime time);
         ~ShmemReq();

         ShmemMsg* getShmemMsg() { return m_shmem_msg; }
         SubsecondTime getTime() { return m_time; }
         
         void setTime(SubsecondTime time) { m_time = time; }
         void updateTime(SubsecondTime time)
         {
            if (time > m_time)
               m_time = time;
         }
   };

}
