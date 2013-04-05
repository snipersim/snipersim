#ifndef __SCHEDULER_ROUND_ROBIN_H
#define __SCHEDULER_ROUND_ROBIN_H

#include "scheduler_dynamic.h"

class SchedulerPinned : public SchedulerDynamic
{
   public:
      SchedulerPinned(ThreadManager *thread_manager);

      virtual core_id_t threadCreate(thread_id_t);
      virtual void threadYield(thread_id_t thread_id);
      virtual bool threadSetAffinity(thread_id_t calling_thread_id, thread_id_t thread_id, size_t cpusetsize, const cpu_set_t *mask);
      virtual bool threadGetAffinity(thread_id_t thread_id, size_t cpusetsize, cpu_set_t *mask);

      virtual void periodic(SubsecondTime time);
      virtual void threadStart(thread_id_t thread_id, SubsecondTime time);
      virtual void threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time);
      virtual void threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time);
      virtual void threadExit(thread_id_t thread_id, SubsecondTime time);

   private:
      struct ThreadInfo
      {
         ThreadInfo() : core_affinity(INVALID_CORE_ID), core_running(INVALID_CORE_ID) {}
         core_id_t core_affinity;
         core_id_t core_running;
      };

      // Configuration
      const SubsecondTime m_quantum;
      const int m_interleaving;
      std::vector<bool> m_core_mask;
      // Global state
      core_id_t m_next_core;
      SubsecondTime m_last_periodic;
      // Keyed by thread_id
      std::vector<ThreadInfo> m_thread_info;
      // Keyed by core_id
      std::vector<thread_id_t> m_core_thread_running;
      std::vector<SubsecondTime> m_quantum_left;

      core_id_t getNextCore(core_id_t core_id);
      void reschedule(SubsecondTime time, core_id_t core_id, bool is_periodic);
      void printState();
};

#endif // __SCHEDULER_ROUND_ROBIN_H
