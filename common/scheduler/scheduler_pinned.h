#ifndef __SCHEDULER_PINNED_H
#define __SCHEDULER_PINNED_H

#include "scheduler_pinned_base.h"

class SchedulerPinned : public SchedulerPinnedBase
{
   public:
      SchedulerPinned(ThreadManager *thread_manager);

      virtual void threadSetInitialAffinity(thread_id_t thread_id);

   private:
      core_id_t getNextCore(core_id_t core_id);

      const int m_interleaving;
      std::vector<bool> m_core_mask;

      core_id_t m_next_core;
};

#endif // __SCHEDULER_ROAMING_H
