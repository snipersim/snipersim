#ifndef __SCHEDULER_STATIC_H
#define __SCHEDULER_STATIC_H

#include "scheduler.h"

#include <vector>

class SchedulerStatic : public Scheduler
{
   public:
      SchedulerStatic(ThreadManager *thread_manager);

      core_id_t threadCreate(thread_id_t);
      virtual bool threadSetAffinity(thread_id_t calling_thread_id, thread_id_t thread_id, size_t cpusetsize, const cpu_set_t *mask) override;
      virtual bool threadGetAffinity(thread_id_t thread_id, size_t cpusetsize, cpu_set_t *mask) override;

   private:
      std::vector<bool> m_core_mask;
      core_id_t findFirstFreeMaskedCore();
};

#endif // __SCHEDULER_STATIC_H
