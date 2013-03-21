#ifndef __ROUTINE_TRACER_FUNCSTATS_H
#define __ROUTINE_TRACER_FUNCSTATS_H

#include "routine_tracer.h"
#include "thread_stats_manager.h"

#include <unordered_map>

class StatsMetricBase;

class RoutineTracerFunctionStats
{
   public:
      typedef std::unordered_map<ThreadStatsManager::ThreadStatType, UInt64> RtnValues;
      class Routine : public RoutineTracer::Routine
      {
         public:
            UInt64 m_calls;
            RtnValues m_values;
            UInt64 m_bits_used, m_bits_total;

            Routine(IntPtr eip, const char *name, int column, int line, const char *filename)
            : RoutineTracer::Routine(eip, name, column, line, filename)
            , m_calls(0), m_values(), m_bits_used(0), m_bits_total(0)
            {}
      };

      class RtnThread;
      class RtnMaster : public RoutineTracer
      {
         public:
            //std::vector<ThreadStatsManager::ThreadStatType> m_threadstats;
            std::unordered_map<thread_id_t, RtnThread*> m_threads;
            RtnMaster();
            virtual ~RtnMaster();

            virtual RoutineTracerThread* getThreadHandler(Thread *thread);
            virtual void addRoutine(IntPtr eip, const char *name, int column, int line, const char *filename);
            void updateRoutine(IntPtr eip, UInt64 calls, RtnValues values);

         private:
            Lock m_lock;
            typedef std::unordered_map<IntPtr, RoutineTracerFunctionStats::Routine*> RoutineMap;
            RoutineMap m_routines;

            UInt64 ce_get_owner(core_id_t core_id);
            void ce_notify(bool on_roi_end, UInt64 owner, CacheBlockInfo::BitsUsedType bits_used, UInt32 bits_total);

            static UInt64 __ce_get_owner(UInt64 user, core_id_t core_id)
            { return ((RtnMaster*)user)->ce_get_owner(core_id); }
            static void __ce_notify(UInt64 user, bool on_roi_end, UInt64 owner, CacheBlockInfo::BitsUsedType bits_used, UInt32 bits_total)
            { ((RtnMaster*)user)->ce_notify(on_roi_end, owner, bits_used, bits_total); }

            void writeResults(const char *filename);
      };

      class RtnThread : public RoutineTracerThread
      {
         public:
            RtnThread(RtnMaster *master, Thread *thread);
            UInt64 getCurrentRoutineId();

         private:
            RtnMaster *m_master;

            IntPtr m_current_eip;
            RtnValues m_values_start;

            UInt64 getThreadStat(ThreadStatsManager::ThreadStatType type);

         protected:
            virtual void functionEnter(IntPtr eip);
            virtual void functionExit(IntPtr eip);
            virtual void functionChildEnter(IntPtr eip, IntPtr eip_child);
            virtual void functionChildExit(IntPtr eip, IntPtr eip_child);
      };

      class ThreadStatAggregates
      {
         public:
            static void registerStats();
         private:
            enum StatType {
               GLOBAL_INSTRUCTIONS,
               GLOBAL_NONIDLE_ELAPSED_TIME,
            };
            static UInt64 callback(ThreadStatsManager::ThreadStatType type, thread_id_t thread_id, Core *core, UInt64 user);
      };

      class ThreadStatCpiMem
      {
         public:
            static ThreadStatsManager::ThreadStatType registerStat();
         private:
            std::vector<std::vector<StatsMetricBase*> > m_stats;
            ThreadStatCpiMem();
            static UInt64 callback(ThreadStatsManager::ThreadStatType type, thread_id_t thread_id, Core *core, UInt64 user);
      };
};

#endif // __ROUTINE_TRACER_FUNCSTATS_H
