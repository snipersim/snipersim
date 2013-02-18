#ifndef __ROUTINE_TRACER_H
#define __ROUTINE_TRACER_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include "lock.h"

#include <deque>
#include <vector>
#include <unordered_map>
#include <cstring>

class RoutineTracer;
class Thread;
class StatsMetricBase;

class RoutineTracerThreadHandler
{
   public:
      RoutineTracerThreadHandler(RoutineTracer *master, Thread *thread);
      ~RoutineTracerThreadHandler();

      void routineEnter(IntPtr eip);
      void routineExit(IntPtr eip);

   protected:
      RoutineTracer *m_master;
      Thread *m_thread;

   private:
      std::deque<IntPtr> m_stack;

      virtual void functionEnter(IntPtr eip) {}
      virtual void functionExit(IntPtr eip) {}
      virtual void functionChildEnter(IntPtr eip, IntPtr eip_child) {}
      virtual void functionChildExit(IntPtr eip, IntPtr eip_child) {}
};

class RTNRoofline : public RoutineTracerThreadHandler
{
   public:
      RTNRoofline(RoutineTracer *master, Thread *thread);

   private:
      StatsMetricBase *m_stat_fp_addsub, *m_stat_fp_muldiv, *m_stat_l3miss;

      IntPtr m_eip;
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

class RoutineTracer
{
   public:
      class Routine
      {
         public:
            const IntPtr m_eip;
            const char *m_name;
            const char *m_location;

            UInt64 m_calls;
            UInt64 m_instruction_count;
            SubsecondTime m_elapsed_time;
            UInt64 m_fp_instructions;
            UInt64 m_l3_misses;

            Routine(IntPtr eip, const char *name, const char *location)
            : m_eip(eip), m_name(strdup(name)), m_location(strdup(location))
            , m_calls(0), m_instruction_count(0), m_elapsed_time(SubsecondTime::Zero())
            , m_fp_instructions(0), m_l3_misses(0)
            {}
      };

      RoutineTracer();
      ~RoutineTracer();

      void addRoutine(IntPtr eip, const char *name, int column, int line, const char *filename);
      void updateRoutine(IntPtr eip, UInt64 calls, UInt64 instruction_count, SubsecondTime elapsed_time, UInt64 fp_instructions, UInt64 m_misses);
      RoutineTracerThreadHandler* getThreadHandler(Thread *thread);
      void writeResults(const char *filename);

   private:
      Lock m_lock;
      std::vector<RoutineTracerThreadHandler*> m_threads;
      std::unordered_map<IntPtr, Routine*> m_routines;
};

extern RoutineTracer *routine_tracer;

#endif // __ROUTINE_TRACER_H
