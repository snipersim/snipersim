#ifndef __THREAD_STATS_MANAGER_H
#define __THREAD_STATS_MANAGER_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include "hooks_manager.h"

class ThreadStatsManager
{
   public:
      enum ThreadStatType
      {
         INSTRUCTIONS,
         ELAPSED_TIME,
         ELAPSED_NONIDLE_TIME,
         NUM_THREAD_STAT_TYPES
      };
      class ThreadStats
      {
         public:
            const Thread *m_thread;
            core_id_t m_core_id;
            std::vector<UInt64> time_by_core;
            std::vector<UInt64> insn_by_core;
            SubsecondTime m_elapsed_time;
            SubsecondTime m_unscheduled_time;

            ThreadStats(thread_id_t thread_id, SubsecondTime time);
            void update(SubsecondTime time);  // Update statistics

         private:
            SubsecondTime m_time_last;    // Time of last snapshot
            std::vector<UInt64> m_counts; // Running total of thread statistics
            std::vector<UInt64> m_last;   // Snapshot of core's statistics when we last updated m_current
      };
      typedef UInt64 (*ThreadStatCallback)(ThreadStatType type, thread_id_t thread_id, Core *core);

      ThreadStatsManager();
      ~ThreadStatsManager();

      // Thread statistics are updated lazily (on thread move and before statistics writing),
      // call this function to force an update before reading
      void update(thread_id_t thread_id = INVALID_THREAD_ID, SubsecondTime time = SubsecondTime::MaxTime());
      const char* getThreadStatName(ThreadStatType type) { return m_thread_stat_names[type]; }
      ThreadStatCallback getThreadStatCallback(ThreadStatType type) { return m_thread_stat_callbacks[type]; }
      const std::unordered_map<thread_id_t, ThreadStats*>& getThreadStats() { return m_threads_stats; }

      void registerThreadStatMetric(ThreadStatType type, const char* name, ThreadStatCallback func);

private:
      std::unordered_map<thread_id_t, ThreadStats*> m_threads_stats;
      std::vector<const char*> m_thread_stat_names;
      std::vector<ThreadStatCallback> m_thread_stat_callbacks;

      static UInt64 metricCallback(ThreadStatType type, thread_id_t thread_id, Core *core);

      void pre_stat_write();
      core_id_t threadCreate(thread_id_t);
      void threadStart(thread_id_t thread_id, SubsecondTime time);
      void threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time);
      void threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time);
      void threadExit(thread_id_t thread_id, SubsecondTime time);

      // Hook stubs
      static SInt64 hook_pre_stat_write(UInt64 ptr, UInt64)
      { ((ThreadStatsManager*)ptr)->pre_stat_write(); return 0; }
      static SInt64 hook_thread_start(UInt64 ptr, UInt64 _args)
      {
         HooksManager::ThreadTime *args = (HooksManager::ThreadTime *)_args;
         ((ThreadStatsManager*)ptr)->threadStart(args->thread_id, args->time);
         return 0;
      }
      static SInt64 hook_thread_stall(UInt64 ptr, UInt64 _args)
      {
         HooksManager::ThreadStall *args = (HooksManager::ThreadStall *)_args;
         ((ThreadStatsManager*)ptr)->threadStall(args->thread_id, args->reason, args->time);
         return 0;
      }
      static SInt64 hook_thread_resume(UInt64 ptr, UInt64 _args)
      {
         HooksManager::ThreadResume *args = (HooksManager::ThreadResume *)_args;
         ((ThreadStatsManager*)ptr)->threadResume(args->thread_id, args->thread_by, args->time);
         return 0;
      }
      static SInt64 hook_thread_exit(UInt64 ptr, UInt64 _args)
      {
         HooksManager::ThreadTime *args = (HooksManager::ThreadTime *)_args;
         ((ThreadStatsManager*)ptr)->threadExit(args->thread_id, args->time);
         return 0;
      }
};

#endif // __THREAD_STATS_MANAGER_H
