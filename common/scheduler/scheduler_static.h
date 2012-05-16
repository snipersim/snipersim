#ifndef __SCHEDULER_STATIC_H
#define __SCHEDULER_STATIC_H

#include "scheduler.h"

class SchedulerStatic : public Scheduler
{
   public:
      SchedulerStatic(ThreadManager *thread_manager) : Scheduler(thread_manager) {}

      core_id_t threadCreate(thread_id_t);
};

#endif // __SCHEDULER_STATIC_H
