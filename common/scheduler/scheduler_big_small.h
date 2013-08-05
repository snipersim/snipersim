#ifndef __SCHEDULER_BIG_SMALL_H
#define __SCHEDULER_BIG_SMALL_H

#include "scheduler_pinned_base.h"

#include <unordered_map>

class SchedulerBigSmall : public SchedulerPinnedBase
{
   public:
      SchedulerBigSmall(ThreadManager *thread_manager);

      virtual void threadSetInitialAffinity(thread_id_t thread_id);
      virtual void threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time);
      virtual void threadExit(thread_id_t thread_id, SubsecondTime time);
      virtual void periodic(SubsecondTime time);

   private:
      const bool m_debug_output;

      // Configuration
      UInt64 m_num_big_cores;
      cpu_set_t m_mask_big;
      cpu_set_t m_mask_small;

      SubsecondTime m_last_reshuffle;
      UInt64 m_rng;
      std::unordered_map<thread_id_t, bool> m_thread_isbig;

      void moveToBig(thread_id_t thread_id);
      void moveToSmall(thread_id_t thread_id);
      void pickBigThread();
};

#endif // __SCHEDULER_BIG_SMALL_H
