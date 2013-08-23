#include "routine_tracer_ondemand.h"
#include "simulator.h"
#include "thread_manager.h"
#include "thread.h"

#include <signal.h>

void RoutineTracerOndemand::RtnThread::printStack()
{
   Core::State state = Sim()->getThreadManager()->getThreadState(m_thread->getId());
   printf("Thread %d (app %d): %s", m_thread->getId(), m_thread->getAppId(), Core::CoreStateString(state));
   if (m_thread->getCore())
      printf(" on core %d", m_thread->getCore()->getId());
   printf("\n");
   for(std::deque<IntPtr>::reverse_iterator it = m_stack.rbegin(); it != m_stack.rend(); ++it)
   {
      printf("\t(%12" PRIxPTR ") %s\n", *it, m_master->getRoutine(*it) ? m_master->getRoutine(*it)->m_name : "(unknown)");
   }
   printf("\n");
}

RoutineTracerOndemand::RtnMaster::RtnMaster()
{
   signal(SIGUSR1, signalHandler);
}

void RoutineTracerOndemand::RtnMaster::signalHandler(int signal)
{
   ScopedLock sl(Sim()->getThreadManager()->getLock());

   for(thread_id_t thread_id = 0; thread_id < (thread_id_t)Sim()->getThreadManager()->getNumThreads(); ++thread_id)
   {
      Thread *thread = Sim()->getThreadManager()->getThreadFromID(thread_id);
      RoutineTracerThread *tracer = thread->getRoutineTracer();
      RoutineTracerOndemand::RtnThread *ondemand_tracer = dynamic_cast<RoutineTracerOndemand::RtnThread*>(tracer);
      LOG_ASSERT_ERROR(ondemand_tracer, "Expected a routine tracer of type RoutineTracerOndemand::RtnThread");

      ondemand_tracer->printStack();
   }
}

void RoutineTracerOndemand::RtnMaster::addRoutine(IntPtr eip, const char *name, const char *imgname, IntPtr offset, int column, int line, const char *filename)
{
   ScopedLock sl(m_lock);

   if (m_routines.count(eip) == 0)
   {
      m_routines[eip] = new RoutineTracer::Routine(eip, name, imgname, offset, column, line, filename);
   }
}

bool RoutineTracerOndemand::RtnMaster::hasRoutine(IntPtr eip)
{
   ScopedLock sl(m_lock);
   return m_routines.count(eip) > 0;
}

RoutineTracer::Routine* RoutineTracerOndemand::RtnMaster::getRoutine(IntPtr eip)
{
   ScopedLock sl(m_lock);
   return m_routines[eip];
}
