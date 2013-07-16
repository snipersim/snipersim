#include "shmem_req.h"
#include "log.h"

namespace PrL1PrL2DramDirectoryMSI
{
   ShmemReq::ShmemReq(ShmemMsg* shmem_msg, SubsecondTime time)
      : m_time(time)
      , m_wait_for_data(false)
      , m_forwarding_from(INVALID_CORE_ID)
   {
      // Make a local copy of the shmem_msg
      m_shmem_msg = new ShmemMsg(shmem_msg);
      LOG_ASSERT_ERROR(shmem_msg->getDataBuf() == NULL,
            "Shmem Reqs should not have data payloads");
   }

   ShmemReq::~ShmemReq()
   {
      delete m_shmem_msg;
   }
}
