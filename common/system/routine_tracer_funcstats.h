#ifndef __ROUTINE_TRACER_FUNCSTATS_H
#define __ROUTINE_TRACER_FUNCSTATS_H

#include "routine_tracer.h"
#include "thread_stats_manager.h"
#include "cache_efficiency_tracker.h"

#include <unordered_map>

class StatsMetricBase;

class RoutineTracerFunctionStats
{
   public:
      typedef std::unordered_map<ThreadStatsManager::ThreadStatType, UInt64> RtnValues;
      class Routine : public RoutineTracer::Routine
      {
         public:
            bool m_provisional;
            UInt64 m_calls;
            RtnValues m_values;
            UInt64 m_bits_used, m_bits_total;

            Routine(IntPtr eip, const char *name, const char *imgname, IntPtr offset, int column, int line, const char *filename)
            : RoutineTracer::Routine(eip, name, imgname, offset, column, line, filename)
            , m_provisional(false), m_calls(0), m_values(), m_bits_used(0), m_bits_total(0)
            {}

            bool isProvisional() const { return m_provisional; }
            void setProvisional(bool provisional) { m_provisional = provisional; }

            // The superclass data is copied, but clear the statistics.
            Routine(const Routine &r)
            : RoutineTracer::Routine(r)
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
            virtual void addRoutine(IntPtr eip, const char *name, const char *imgname, IntPtr offset, int column, int line, const char *filename);
            virtual bool hasRoutine(IntPtr eip);
            void updateRoutine(IntPtr eip, UInt64 calls, RtnValues values);
            void updateRoutineFull(const CallStack& stack, UInt64 calls, RtnValues values);
            void updateRoutineFull(RoutineTracerFunctionStats::Routine* rtn, UInt64 calls, RtnValues values);
            RoutineTracerFunctionStats::Routine* getRoutineFullPtr(const CallStack& stack);

         private:
            Lock m_lock;
            // Flat-profile per-thread statistics (excludes statistics from child calls).
            typedef std::unordered_map<IntPtr, RoutineTracerFunctionStats::Routine*> RoutineMap;
            RoutineMap m_routines;
            // Call-stack-based statistics (includes statistics from child calls).
            typedef std::unordered_map<CallStack, RoutineTracerFunctionStats::Routine*> RoutineMapFull;
            RoutineMapFull m_callstack_routines;

            UInt64 ce_get_owner(core_id_t core_id, UInt64 address);
            void ce_notify_evict(bool on_roi_end, UInt64 owner, UInt64 evictor, CacheBlockInfo::BitsUsedType bits_used, UInt32 bits_total);

            static UInt64 __ce_get_owner(UInt64 user, core_id_t core_id, UInt64 address)
            { return ((RtnMaster*)user)->ce_get_owner(core_id, address); }
            static void __ce_notify_evict(UInt64 user, bool on_roi_end, UInt64 owner, UInt64 evictor, CacheBlockInfo::BitsUsedType bits_used, UInt32 bits_total)
            { ((RtnMaster*)user)->ce_notify_evict(on_roi_end, owner, evictor, bits_used, bits_total); }

            void writeResults(const char *filename);
            void writeResultsFull(const char *filename);
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
            std::unordered_map<CallStack, RtnValues> m_values_start_full;

            void functionBegin(IntPtr eip);
            void functionEnd(IntPtr eip, bool is_function_start);

            void functionBeginHelper(IntPtr eip, RtnValues&);
            void functionEndHelper(IntPtr eip, UInt64 count);
            void functionEndFullHelper(const CallStack &stack, UInt64 count);

            UInt64 getThreadStat(ThreadStatsManager::ThreadStatType type);

         protected:
            virtual void functionEnter(IntPtr eip, IntPtr callEip);
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
