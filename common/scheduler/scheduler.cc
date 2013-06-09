#include "scheduler.h"
#include "scheduler_static.h"
#include "scheduler_pinned.h"
#include "scheduler_roaming.h"
#include "scheduler_big_small.h"
#include "simulator.h"
#include "config.hpp"
#include "core_manager.h"
#include "thread_manager.h"
#include "thread.h"

Scheduler* Scheduler::create(ThreadManager *thread_manager)
{
   String type = Sim()->getCfg()->getString("scheduler/type");

   if (type == "static")
      return new SchedulerStatic(thread_manager);
   else if (type == "pinned")
      return new SchedulerPinned(thread_manager);
   else if (type == "roaming")
      return new SchedulerRoaming(thread_manager);
   else if (type == "big_small")
      return new SchedulerBigSmall(thread_manager);
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


void Scheduler::printMapping()
{
   // Print mapping
   for (core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); core_id++)
   {
      if (Sim()->getCoreManager()->getCoreFromID(core_id)->getState() != Core::IDLE)
      {
         char state;
         switch(Sim()->getThreadManager()->getThreadState(Sim()->getCoreManager()->getCoreFromID(core_id)->getThread()->getId() ))
         {
            case Core::INITIALIZING:
               state = 'I';
               break;
            case Core::RUNNING:
               state = 'R';
               break;
            case Core::STALLED:
               state = 'S';
               break;
            case Core::SLEEPING:
               state = 's';
               break;
            case Core::WAKING_UP:
               state = 'W';
               break;
            case Core::IDLE:
               state = 'i';
               break;
            case Core::BROKEN:
               state = 'B';
               break;
            case Core::NUM_STATES:
            default:
               state = '?';
               break;
         }
         std::cout << "( t" << Sim()->getCoreManager()->getCoreFromID(core_id)->getThread()->getId() << "@c" << core_id << "[" << state << "])" ;
      }
      else
      {
         std::cout << "( tx" << "@c" << core_id  << "[i])";
      }
   }
   std::cout << std::endl;
}
