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
      virtual void threadYield(thread_id_t thread_id) {}
      virtual bool threadSetAffinity(thread_id_t calling_thread_id, thread_id_t thread_id, size_t cpusetsize, const cpu_set_t *mask) { return false; }
      virtual bool threadGetAffinity(thread_id_t thread_id, size_t cpusetsize, cpu_set_t *mask) { return false; }

   protected:
      ThreadManager *m_thread_manager;

      // Utility functions
      core_id_t findFirstFreeCore();
      void printMapping();
};

#endif // __SCHEDULER_H
