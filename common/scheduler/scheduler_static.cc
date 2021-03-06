#include "scheduler_static.h"
#include "thread_manager.h"
#include "core_manager.h"
#include "simulator.h"
#include "config.hpp"
#include "thread.h"
#include "log.h"

SchedulerStatic::SchedulerStatic(ThreadManager *thread_manager)
   : Scheduler(thread_manager)
{
   m_core_mask.resize(Sim()->getConfig()->getApplicationCores());

   for (core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); core_id++)
   {
       m_core_mask[core_id] = Sim()->getCfg()->getBoolArray("scheduler/static/core_mask", core_id);
   }
}

core_id_t SchedulerStatic::findFirstFreeMaskedCore()
{
   for (core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); core_id++)
   {
      if (m_core_mask[core_id] && Sim()->getCoreManager()->getCoreFromID(core_id)->getState() == Core::IDLE)
      {
         return core_id;
      }
   }
   return INVALID_CORE_ID;
}

core_id_t SchedulerStatic::threadCreate(thread_id_t thread_id)
{
   core_id_t core_id = findFirstFreeMaskedCore();
   LOG_ASSERT_ERROR(core_id != INVALID_CORE_ID, "No cores available for spawnThread request.");

   app_id_t app_id = Sim()->getThreadManager()->getThreadFromID(thread_id)->getAppId();
   LOG_PRINT("Scheduler: thread %d from application %d now scheduled to core %d", thread_id, app_id, core_id);

   return core_id;
}

bool SchedulerStatic::threadSetAffinity(thread_id_t calling_thread_id, thread_id_t thread_id, size_t cpusetsize, const cpu_set_t *mask)
{
   // We can't accept SetAffinitity requests with this scheduler. Silently ignore the request.
   return true;
}

bool SchedulerStatic::threadGetAffinity(thread_id_t thread_id, size_t cpusetsize, cpu_set_t *mask)
{
   if (cpusetsize*8 < Sim()->getConfig()->getApplicationCores())
   {
      // Not enough space to return result
      return false;
   }

   CPU_ZERO_S(cpusetsize, mask);

   Core *core = Sim()->getThreadManager()->getThreadFromID(thread_id)->getCore();
   if (core == NULL)
   {
      return false;
   }

   core_id_t core_id = core->getId();

   CPU_SET_S(core_id, cpusetsize, mask);

   return true;
}
