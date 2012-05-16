#include "scheduler_static.h"
#include "log.h"

core_id_t SchedulerStatic::threadCreate(thread_id_t)
{
   core_id_t core_id = findFirstFreeCore();

   LOG_ASSERT_ERROR(core_id != INVALID_CORE_ID, "No cores available for spawnThread request.");

   return core_id;
}
