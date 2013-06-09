#ifndef __SCHEDULER_ROAMING_H
#define __SCHEDULER_ROAMING_H

#include "scheduler_pinned.h"

class SchedulerRoaming : public SchedulerPinnedBase
{
   public:
      SchedulerRoaming(ThreadManager *thread_manager);

      virtual void threadSetInitialAffinity(thread_id_t thread_id);

   private:
      std::vector<bool> m_core_mask;
};

#endif // __SCHEDULER_ROAMING_H
