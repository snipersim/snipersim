#ifndef __THREAD_STATS_MANAGER_H
#define __THREAD_STATS_MANAGER_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include "hooks_manager.h"
#include "bottlegraph.h"

class StatsMetricBase;

class ThreadStatsManager
{
   public:
      typedef UInt32 ThreadStatType;
      typedef std::vector<ThreadStatType> ThreadStatTypeList;
      enum ThreadStatTypeEnum
      {
         INSTRUCTIONS,
         ELAPSED_NONIDLE_TIME,
         WAITING_COST,
         NUM_THREAD_STAT_FIXED_TYPES,  // Number of fixed thread statistics
         DYNAMIC,                      // User-defined thread statistics
         INVALID
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

            ThreadStats(thread_id_t thread_id);
            void update(SubsecondTime time, bool init = false);  // Update statistics

         private:
            SubsecondTime m_time_last;    // Time of last snapshot
            std::unordered_map<ThreadStatType, UInt64> m_counts; // Running total of thread statistics
            std::unordered_map<ThreadStatType, UInt64> m_last;   // Snapshot of core's statistics when we last updated m_current

            friend class ThreadStatsManager;
      };
      typedef UInt64 (*ThreadStatCallback)(ThreadStatType type, thread_id_t thread_id, Core *core, UInt64 user);

      ThreadStatsManager();
      ~ThreadStatsManager();

      // Thread statistics are updated lazily (on thread move and before statistics writing),
      // call this function to force an update before reading
      void update(thread_id_t thread_id = INVALID_THREAD_ID, SubsecondTime time = SubsecondTime::MaxTime());
      void calculateWaitingCosts(SubsecondTime time);

      const ThreadStatTypeList& getThreadStatTypes() { return m_thread_stat_types; }
      const char* getThreadStatName(ThreadStatType type) { return m_thread_stat_callbacks[type].m_name; }
      UInt64 getThreadStatistic(thread_id_t thread_id, ThreadStatType type) { return m_threads_stats[thread_id]->m_counts[type]; }

      ThreadStatType registerThreadStatMetric(ThreadStatType type, const char* name, ThreadStatCallback func, UInt64 user);

private:
      struct StatCallback {
         const char* m_name;
         ThreadStatCallback m_func;
         UInt64 m_user;

         StatCallback() {};
         StatCallback(const char* name, ThreadStatCallback func, UInt64 user) : m_name(name), m_func(func), m_user(user) {}
         UInt64 call(ThreadStatType type, thread_id_t thread_id, Core *core) { return m_func(type, thread_id, core, m_user); }
      };
      // Make sure m_threads_stats is statically allocated, as we may do inserts and reads simultaneously
      // which does not work on an unordered_map
      static const int MAX_THREADS = 4096;
      std::vector<ThreadStats*> m_threads_stats;
      ThreadStatTypeList m_thread_stat_types;
      std::unordered_map<ThreadStatType, StatCallback> m_thread_stat_callbacks;
      ThreadStatType m_next_dynamic_type;
      BottleGraphManager m_bottlegraphs;
      SubsecondTime m_waiting_time_last;

      static UInt64 metricCallback(ThreadStatType type, thread_id_t thread_id, Core *core, UInt64 user);
      UInt64 callThreadStatCallback(ThreadStatType type, thread_id_t thread_id, Core *core);

      void pre_stat_write();
      void threadCreate(thread_id_t thread_id);
      void threadStart(thread_id_t thread_id, SubsecondTime time);
      void threadStall(thread_id_t thread_id, ThreadManager::stall_type_t reason, SubsecondTime time);
      void threadResume(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time);
      void threadExit(thread_id_t thread_id, SubsecondTime time);

      // Hook stubs
      static SInt64 hook_pre_stat_write(UInt64 ptr, UInt64)
      { ((ThreadStatsManager*)ptr)->pre_stat_write(); return 0; }
      static SInt64 hook_thread_create(UInt64 ptr, UInt64 _args)
      {
         HooksManager::ThreadCreate *args = (HooksManager::ThreadCreate *)_args;
         ((ThreadStatsManager*)ptr)->threadCreate(args->thread_id);
         return 0;
      }
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

// Helper class to register named statistics objects as per-thread statistics.
// Caches the StatsMetricBase objects to avoid the string lookups on every access.
class ThreadStatNamedStat
{
   public:
      static ThreadStatsManager::ThreadStatType registerStat(const char* name, String objectName, String metricName);
   private:
      std::vector<StatsMetricBase*> m_stats;
      ThreadStatNamedStat(String objectName, String metricName);
      static UInt64 callback(ThreadStatsManager::ThreadStatType type, thread_id_t thread_id, Core *core, UInt64 user);
};

#endif // __THREAD_STATS_MANAGER_H
