#include "routine_tracer_print.h"
#include "thread.h"

RoutineTracerPrint::RtnThread::RtnThread(RoutineTracerPrint::RtnMaster *master, Thread *thread)
   : RoutineTracerThread(thread)
   , m_master(master)
   , m_depth(0)
{
}

void RoutineTracerPrint::RtnThread::functionEnter(IntPtr eip)
{
   printf("[%2d] ", m_thread->getId());
   for(unsigned int i = 0; i < m_depth; ++i)
     printf("  ");
   printf("(%8" PRIxPTR ") %s\n", eip, m_master->getRoutine(eip) ? m_master->getRoutine(eip)->m_name : "(unknown)");
   ++m_depth;
}

void RoutineTracerPrint::RtnThread::functionExit(IntPtr eip)
{
   --m_depth;
}

RoutineTracerThread* RoutineTracerPrint::RtnMaster::getThreadHandler(Thread *thread)
{
   return new RtnThread(this, thread);
}

void RoutineTracerPrint::RtnMaster::addRoutine(IntPtr eip, const char *name, const char *imgname, IntPtr offset, int column, int line, const char *filename)
{
   ScopedLock sl(m_lock);

   if (m_routines.count(eip) == 0)
   {
      m_routines[eip] = new RoutineTracer::Routine(eip, name, imgname, offset, column, line, filename);
   }
}

bool RoutineTracerPrint::RtnMaster::hasRoutine(IntPtr eip)
{
   ScopedLock sl(m_lock);

   return m_routines.count(eip) > 0;
}

RoutineTracer::Routine* RoutineTracerPrint::RtnMaster::getRoutine(IntPtr eip)
{
   ScopedLock sl(m_lock);
   return m_routines[eip];
}
