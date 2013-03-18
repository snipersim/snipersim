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

            Routine(IntPtr eip, const char *name, int column, int line, const char *filename)
            : RoutineTracer::Routine(eip, name, column, line, filename)
            , m_calls(0), m_values()
            {}
      };

      class RtnMaster : public RoutineTracer
      {
         public:
            //std::vector<ThreadStatsManager::ThreadStatType> m_threadstats;
            RtnMaster();
            virtual ~RtnMaster();

            virtual RoutineTracerThread* getThreadHandler(Thread *thread);
            virtual void addRoutine(IntPtr eip, const char *name, int column, int line, const char *filename);
            void updateRoutine(IntPtr eip, UInt64 calls, RtnValues values);

         private:
            Lock m_lock;
            std::unordered_map<IntPtr, RoutineTracerFunctionStats::Routine*> m_routines;

            void writeResults(const char *filename);
      };

      class RtnThread : public RoutineTracerThread
      {
         public:
            RtnThread(RtnMaster *master, Thread *thread);

         private:
            RtnMaster *m_master;

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
};

#endif // __ROUTINE_TRACER_FUNCSTATS_H
