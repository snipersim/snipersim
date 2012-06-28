#ifndef __SCHEDULER_H
#define __SCHEDULER_H

#include "fixed_types.h"
#include "thread_manager.h"

class Scheduler
{
   public:
      static Scheduler* create(ThreadManager *thread_manager);

      Scheduler(ThreadManager *thread_manager);
      virtual ~Scheduler() {}

      virtual core_id_t threadCreate(thread_id_t thread_id) = 0;
      virtual void threadSetAffinity(thread_id_t thread_id, size_t cpusetsize, const cpu_set_t *mask) {}

   protected:
      ThreadManager *m_thread_manager;

      // Utility functions
      core_id_t findFirstFreeCore();
};

#endif // __SCHEDULER_H
