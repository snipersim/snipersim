#ifndef __SCHEDULER_RAND_H
#define __SCHEDULER_RAND_H

#include "scheduler_dynamic.h"

#include <map>

class SchedulerRand : public SchedulerDynamic
{
   public:
      SchedulerRand(ThreadManager *thread_manager);

      virtual core_id_t threadCreate(thread_id_t);
      virtual void periodic(SubsecondTime time);
      virtual void threadStart(thread_id_t thread_id, SubsecondTime time);
      virtual void threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time);
      virtual void threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time);
      virtual void threadExit(thread_id_t thread_id, SubsecondTime time);

   private:
      std::map<core_id_t, thread_id_t> m_coreToThreadMapping;

      uint64_t m_nBigCores;
      uint64_t m_nSmallCores;

      SubsecondTime m_quantum;
      SubsecondTime m_quantum_left;
      SubsecondTime m_last_periodic;

      void reschedule(SubsecondTime time);
};

#endif // __SCHEDULER_RAND_H
