#ifndef __ROUTINE_TRACER_H
#define __ROUTINE_TRACER_H

#include "fixed_types.h"
#include "lock.h"

#include <deque>
#include <vector>
#include <unordered_map>
#include <cstring>

class RoutineTracer;

class RoutineTracerThreadHandler
{
   public:
      RoutineTracerThreadHandler(RoutineTracer *master);
      ~RoutineTracerThreadHandler();

      void routineEnter(IntPtr eip);
      void routineExit(IntPtr eip);

   private:
      RoutineTracer *m_master;
      std::deque<IntPtr> m_stack;

      void functionEnter(IntPtr eip);
      void functionExit(IntPtr eip);
      void functionChildEnter(IntPtr eip);
      void functionChildExit(IntPtr eip);
};

class RoutineTracer
{
   public:
      class Routine
      {
         public:
            IntPtr m_eip;
            const char *m_name;
            const char *m_location;

            Routine(IntPtr eip, const char *name, const char *location)
            : m_eip(eip), m_name(strdup(name)), m_location(strdup(location)) {}
      };

      RoutineTracer();
      ~RoutineTracer();

      void addRoutine(IntPtr eip, const char *name, int column, int line, const char *filename);
      RoutineTracerThreadHandler* getThreadHandler();
      void writeResults(const char *filename);

   private:
      Lock m_lock;
      std::vector<RoutineTracerThreadHandler*> m_threads;
      std::unordered_map<IntPtr, Routine*> m_routines;
};

extern RoutineTracer *routine_tracer;

#endif // __ROUTINE_TRACER_H
