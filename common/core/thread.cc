#include "thread.h"
#include "simulator.h"
#include "syscall_model.h"
#include "thread_manager.h"
#include "core_manager.h"
#include "sync_client.h"
#include "performance_model.h"
#include "routine_tracer.h"
#include "config.hpp"

Thread::Thread(thread_id_t thread_id, app_id_t app_id)
   : m_thread_id(thread_id)
   , m_app_id(app_id)
   , m_core(NULL)
   , m_rtn_tracer(NULL)
   , m_va2pa_func(NULL)
   , m_va2pa_arg(0)
{
   m_syscall_model = new SyscallMdl(this);
   m_sync_client = new SyncClient(this);
   if (Sim()->getRoutineTracer())
      m_rtn_tracer = Sim()->getRoutineTracer()->getThreadHandler(this);
   memset(&m_os_info, 0, sizeof(m_os_info));
}

Thread::~Thread()
{
   delete m_syscall_model;
   delete m_sync_client;
   if (m_rtn_tracer)
      delete m_rtn_tracer;
}

void Thread::setCore(Core* core)
{
   if (m_core)
      m_core->setThread(NULL);

   m_core = core;

   if (m_core)
   {
      LOG_ASSERT_ERROR(core->getThread() == NULL, "Cannot move thread %d to core %d as it is already running thread %d", getId(), core->getId(), core->getThread()->getId());
      m_core->setThread(this);
   }
}

void Thread::setVa2paFunc(va2pa_func_t va2pa_func, UInt64 va2pa_arg)
{
   m_va2pa_func = va2pa_func;
   m_va2pa_arg = va2pa_arg;
}

bool Thread::reschedule(SubsecondTime &time, Core *current_core)
{
   if (m_core == NULL || current_core != m_core)
   {
      // While we're not scheduled on a core, wait.
      // Keep time updated with the time when we return.
      ScopedLock sl(Sim()->getThreadManager()->getLock());

      // Do a definitive check for m_core, while holding the lock, unoptimized because another thread will be changing it
      while((volatile Core*)m_core == NULL)
         time = Sim()->getThreadManager()->stallThread(getId(), ThreadManager::STALL_UNSCHEDULED, time);

      m_core->getPerformanceModel()->queueDynamicInstruction(new SyncInstruction(time, SyncInstruction::UNSCHEDULED));

      updateCoreTLS();
      return true;
   }
   else
      return false;
}

bool Thread::updateCoreTLS(int threadIndex)
{
   if (Sim()->getCoreManager()->getCurrentCore(threadIndex) != m_core)
   {
      if (Sim()->getCoreManager()->getCurrentCore(threadIndex))
         Sim()->getCoreManager()->terminateThread();
      if (m_core)
         Sim()->getCoreManager()->initializeThread(m_core->getId());
      return true;
   }
   else
      return false;
}
