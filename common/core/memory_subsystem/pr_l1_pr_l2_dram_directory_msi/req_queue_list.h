#pragma once

#include <map>
#include <queue>

#include "shmem_req.h"
#include "req_queue_list_template.h"

namespace PrL1PrL2DramDirectoryMSI
{
  typedef ReqQueueListTemplate<ShmemReq> ReqQueueList;
}
