#include "routine_tracer.h"

RoutineTracer *routine_tracer = NULL;

RoutineTracerThreadHandler::RoutineTracerThreadHandler(RoutineTracer *master)
   : m_master(master)
{
}

RoutineTracerThreadHandler::~RoutineTracerThreadHandler()
{
}

void RoutineTracerThreadHandler::routineEnter(IntPtr eip)
{
   if (m_stack.size())
      functionChildEnter(m_stack.back());

   m_stack.push_back(eip);
   functionEnter(eip);
}

void RoutineTracerThreadHandler::routineExit(IntPtr eip)
{
   if (m_stack.back() == eip)
   {
      functionExit(eip);
      m_stack.pop_back();
   }
   else
   {
      bool found = false;
      for(auto it = m_stack.rbegin(); it != m_stack.rend(); ++it)
      {
         if (*it == eip)
         {
            // We found this eip further down the stack: unwind
            while(m_stack.back() != eip)
            {
               functionExit(m_stack.back());
               m_stack.pop_back();
            }
            found = true;
            break;
         }
      }
      if (!found)
      {
         // Mismatch, ignore
      }
   }

   if (m_stack.size())
      functionChildExit(m_stack.back());
}

void RoutineTracerThreadHandler::functionEnter(IntPtr eip)
{
}

void RoutineTracerThreadHandler::functionExit(IntPtr eip)
{
}

void RoutineTracerThreadHandler::functionChildEnter(IntPtr eip)
{
}

void RoutineTracerThreadHandler::functionChildExit(IntPtr eip)
{
}


RoutineTracer::RoutineTracer()
{
}

RoutineTracer::~RoutineTracer()
{
}

void RoutineTracer::addRoutine(IntPtr eip, const char *name, int column, int line, const char *filename)
{
   ScopedLock sl(m_lock);

   if (m_routines.count(eip) == 0)
   {
      char location[1024];
      snprintf(location, 1023, "%s:%d:%d", filename, line, column);
      location[1023] = '\0';

      m_routines[eip] = new Routine(eip, name, location);
   }
}

RoutineTracerThreadHandler* RoutineTracer::getThreadHandler()
{
   ScopedLock sl(m_lock);

   RoutineTracerThreadHandler *rtn_thread = new RoutineTracerThreadHandler(this);
   m_threads.push_back(rtn_thread);
   return rtn_thread;
}

void RoutineTracer::writeResults(const char *filename)
{
   FILE *fp = fopen(filename, "w");
   for(auto it = m_routines.begin(); it != m_routines.end(); ++it)
   {
      fprintf(fp, "RTN %lx %s %s\n", it->second->m_eip, it->second->m_name, it->second->m_location);
   }
   fclose(fp);
}
