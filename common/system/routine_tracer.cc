#include "routine_tracer.h"
#include "simulator.h"
#include "config.hpp"
#include "routine_tracer_print.h"
#include "routine_tracer_funcstats.h"

#include <cstring>

RoutineTracerThread::RoutineTracerThread(Thread *thread)
   : m_thread(thread)
{
}

RoutineTracerThread::~RoutineTracerThread()
{
}

void RoutineTracerThread::routineEnter(IntPtr eip)
{
   if (m_stack.size())
      functionChildEnter(m_stack.back(), eip);

   m_stack.push_back(eip);
   functionEnter(eip);
}

void RoutineTracerThread::routineExit(IntPtr eip)
{
   if (m_stack.size() == 0)
      return;

   bool found = false;
   if (m_stack.back() == eip)
   {
      found = true;
   }
   else
   {
      for(std::deque<IntPtr>::reverse_iterator it = m_stack.rbegin(); it != m_stack.rend(); ++it)
      {
         if (*it == eip)
         {
            // We found this eip further down the stack: unwind
            while(m_stack.back() != eip)
            {
               functionExit(m_stack.back());
               m_stack.pop_back();
               functionChildExit(m_stack.back(), eip);
            }
            found = true;
            break;
         }
      }
   }
   if (!found)
   {
      // Mismatch, ignore
   }
   else
   {
      // Unwound into eip, now exit it
      functionExit(eip);
      m_stack.pop_back();
   }

   if (m_stack.size())
      functionChildExit(m_stack.back(), eip);
}

RoutineTracer::Routine::Routine(IntPtr eip, const char *name, int column, int line, const char *filename)
   : m_eip(eip), m_name(strdup(name))
{
   char location[1024];
   snprintf(location, 1023, "%s:%d:%d", filename, line, column);
   location[1023] = '\0';
   m_location = strdup(location);
}

RoutineTracer::RoutineTracer()
{
}

RoutineTracer::~RoutineTracer()
{
}

RoutineTracer* RoutineTracer::create()
{
   String type = Sim()->getCfg()->getString("routine_tracer/type");

   if (type == "none")
      return NULL;
   else if (type == "print")
      return new RoutineTracerPrint::RtnMaster();
   else if (type == "funcstats")
      return new RoutineTracerFunctionStats::RtnMaster();
   else
      LOG_PRINT_ERROR("Unknown routine tracer type %s", type.c_str());
}
