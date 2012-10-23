#ifndef __SCHEDULER_STATIC_MASK_H
#define __SCHEDULER_STATIC_MASK_H

#include <vector>

#include "scheduler.h"

class SchedulerStaticMask : public Scheduler
{
   public:
      SchedulerStaticMask(ThreadManager *thread_manager);

      core_id_t threadCreate(thread_id_t);

   private:
      std::vector<bool> m_core_mask;
      core_id_t findFirstFreeMaskedCore();
};

#endif // __SCHEDULER_STATIC_MASK_H
