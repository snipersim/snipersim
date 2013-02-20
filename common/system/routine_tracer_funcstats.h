#ifndef __ROUTINE_TRACER_FUNCSTATS_H
#define __ROUTINE_TRACER_FUNCSTATS_H

#include "routine_tracer.h"

#include <unordered_map>

class StatsMetricBase;

class RoutineTracerFunctionStats
{
   public:
      class Routine : public RoutineTracer::Routine
      {
         public:
            UInt64 m_calls;
            UInt64 m_instruction_count;
            SubsecondTime m_elapsed_time;
            UInt64 m_fp_instructions;
            UInt64 m_l3_misses;

            Routine(IntPtr eip, const char *name, int column, int line, const char *filename)
            : RoutineTracer::Routine(eip, name, column, line, filename)
            , m_calls(0), m_instruction_count(0), m_elapsed_time(SubsecondTime::Zero())
            , m_fp_instructions(0), m_l3_misses(0)
            {}
      };

      class RtnMaster : public RoutineTracer
      {
         public:
            RtnMaster();
            virtual ~RtnMaster();

            virtual RoutineTracerThread* getThreadHandler(Thread *thread);
            virtual void addRoutine(IntPtr eip, const char *name, int column, int line, const char *filename);
            void updateRoutine(IntPtr eip, UInt64 calls, UInt64 instruction_count, SubsecondTime elapsed_time, UInt64 fp_instructions, UInt64 m_misses);

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
            StatsMetricBase *m_stat_fp_addsub, *m_stat_fp_muldiv, *m_stat_l3miss;

            UInt64 m_calls;
            UInt64 m_instruction_count;
            SubsecondTime m_elapsed_time;
            UInt64 m_fp_instructions;
            UInt64 m_l3_misses;

         protected:
            virtual void functionEnter(IntPtr eip);
            virtual void functionExit(IntPtr eip);
            virtual void functionChildEnter(IntPtr eip, IntPtr eip_child);
            virtual void functionChildExit(IntPtr eip, IntPtr eip_child);
      };
};

#endif // __ROUTINE_TRACER_FUNCSTATS_H
