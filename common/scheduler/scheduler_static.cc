#include "scheduler_static.h"
#include "simulator.h"
#include "thread.h"
#include "log.h"

core_id_t SchedulerStatic::threadCreate(thread_id_t thread_id)
{
   core_id_t core_id = findFirstFreeCore();
   LOG_ASSERT_ERROR(core_id != INVALID_CORE_ID, "No cores available for spawnThread request.");

   app_id_t app_id = Sim()->getThreadManager()->getThreadFromID(thread_id)->getAppId();
   LOG_PRINT("Scheduler: thread %d from application %d now scheduled to core %d", thread_id, app_id, core_id);

   return core_id;
}
