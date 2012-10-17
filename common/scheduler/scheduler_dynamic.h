#ifndef __SCHEDULER_DYNAMIC_H
#define __SCHEDULER_DYNAMIC_H

#include "scheduler.h"
#include "hooks_manager.h"

#include <vector>

class SchedulerDynamic : public Scheduler
{
   public:
      SchedulerDynamic(ThreadManager *thread_manager);
      virtual ~SchedulerDynamic();

      virtual core_id_t threadCreate(thread_id_t) = 0;
      virtual void periodic(SubsecondTime time) {}
      virtual void threadStart(thread_id_t thread_id, SubsecondTime time) {}
      virtual void threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time) {}
      virtual void threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time) {}
      virtual void threadExit(thread_id_t thread_id, SubsecondTime time) {}

      // Thread statistics are updated lazily (on thread move and before statistics writing),
      // call this function to force an update before reading
      void updateThreadStats();

   protected:
      struct ThreadStatsStruct
      {
         UInt64 instructions;
         SubsecondTime elapsed_time;
         SubsecondTime nonidle_elapsed_time;
      };
      class ThreadStats
      {
         public:
            const Thread *m_thread;
            core_id_t m_core_id;
            ThreadStatsStruct m_counts;   // Running total of thread statistics
            SubsecondTime m_elapsed_time;
            SubsecondTime m_unscheduled_time;

            ThreadStats(thread_id_t thread_id, SubsecondTime time);
            void update(SubsecondTime time);  // Update statistics

         private:
            SubsecondTime m_time_last;    // Time of last snapshot
            ThreadStatsStruct m_last;     // Snapshot of core's statistics when we last updated m_current
      };

      std::vector<bool> m_threads_runnable;

      void moveThread(thread_id_t thread_id, core_id_t core_id, SubsecondTime time);
      const std::unordered_map<thread_id_t, ThreadStats*>& getThreadStats() { return m_threads_stats; }

   private:
      std::unordered_map<thread_id_t, ThreadStats*> m_threads_stats;
      bool m_in_periodic;

      void __periodic(SubsecondTime time);
      void __roi_begin();
      void __roi_end();
      void __pre_stat_write();
      void __threadStart(thread_id_t thread_id, SubsecondTime time);
      void __threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time);
      void __threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time);
      void __threadExit(thread_id_t thread_id, SubsecondTime time);

      // Hook stubs
      static SInt64 hook_periodic(UInt64 ptr, UInt64 time)
      { ((SchedulerDynamic*)ptr)->__periodic(*(subsecond_time_t*)&time); return 0; }
      static SInt64 hook_pre_stat_write(UInt64 ptr, UInt64)
      { ((SchedulerDynamic*)ptr)->__pre_stat_write(); return 0; }
      static SInt64 hook_thread_start(UInt64 ptr, UInt64 _args)
      {
         HooksManager::ThreadTime *args = (HooksManager::ThreadTime *)_args;
         ((SchedulerDynamic*)ptr)->__threadStart(args->thread_id, args->time);
         return 0;
      }
      static SInt64 hook_thread_stall(UInt64 ptr, UInt64 _args)
      {
         HooksManager::ThreadStall *args = (HooksManager::ThreadStall *)_args;
         ((SchedulerDynamic*)ptr)->__threadStall(args->thread_id, args->reason, args->time);
         return 0;
      }
      static SInt64 hook_thread_resume(UInt64 ptr, UInt64 _args)
      {
         HooksManager::ThreadResume *args = (HooksManager::ThreadResume *)_args;
         ((SchedulerDynamic*)ptr)->__threadResume(args->thread_id, args->thread_by, args->time);
         return 0;
      }
      static SInt64 hook_thread_exit(UInt64 ptr, UInt64 _args)
      {
         HooksManager::ThreadTime *args = (HooksManager::ThreadTime *)_args;
         ((SchedulerDynamic*)ptr)->__threadExit(args->thread_id, args->time);
         return 0;
      }
};

#endif // __SCHEDULER_DYNAMIC_H
