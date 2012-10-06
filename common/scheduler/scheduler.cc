#include "scheduler.h"
#include "scheduler_static.h"
#include "scheduler_round_robin.h"
#include "scheduler_rand.h"
#include "simulator.h"
#include "config.hpp"
#include "core_manager.h"

Scheduler* Scheduler::create(ThreadManager *thread_manager)
{
   String type = Sim()->getCfg()->getString("scheduler/type");

   if (type == "static")
      return new SchedulerStatic(thread_manager);
   else if (type == "round_robin")
      return new SchedulerRoundRobin(thread_manager);
   else if (type == "rand")
      return new SchedulerRand(thread_manager);
   else
      LOG_PRINT_ERROR("Unknown scheduler type %s", type.c_str());
}

Scheduler::Scheduler(ThreadManager *thread_manager)
   : m_thread_manager(thread_manager)
{
}

core_id_t Scheduler::findFirstFreeCore()
{
   for (core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); core_id++)
   {
      if (Sim()->getCoreManager()->getCoreFromID(core_id)->getState() == Core::IDLE)
      {
         return core_id;
      }
   }
   return INVALID_CORE_ID;
}
