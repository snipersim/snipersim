#include <sys/syscall.h>

#include "thread_manager.h"
#include "core_manager.h"
#include "performance_model.h"
#include "hooks_manager.h"
#include "config.h"
#include "log.h"
#include "transport.h"
#include "simulator.h"
#include "clock_skew_minimization_object.h"
#include "core.h"
#include "thread.h"
#include "scheduler.h"

const char* ThreadManager::stall_type_names[] = {
   "unscheduled", "broken", "join", "mutex", "cond", "barrier", "futex"
};

ThreadManager::ThreadManager()
   : m_thread_tls(TLS::create())
   , m_scheduler(Scheduler::create(this))
{
}

ThreadManager::~ThreadManager()
{
   for (UInt32 i = 0; i < m_thread_state.size(); i++)
   {
      if (m_thread_state[i].status != Core::IDLE)
         fprintf(stderr, "Thread %d still active when ThreadManager destructs\n", i);
      delete m_threads[i];
   }

   delete m_thread_tls;
   delete m_scheduler;
}

Thread* ThreadManager::getThreadFromID(thread_id_t thread_id)
{
   LOG_ASSERT_ERROR((size_t)thread_id < m_threads.size(), "Invalid thread_id %d", thread_id);
   return m_threads.at(thread_id);
}
Thread* ThreadManager::getCurrentThread(int threadIndex)
{
   return m_thread_tls->getPtr<Thread>(threadIndex);
}

Thread* ThreadManager::createThread()
{
   ScopedLock sl(m_thread_lock);
   return createThread_unlocked();
}

Thread* ThreadManager::createThread_unlocked()
{
   thread_id_t thread_id = m_threads.size();
   Thread *thread = new Thread(thread_id);
   m_threads.push_back(thread);
   m_thread_state.push_back(ThreadState());
   m_thread_state[thread->getId()].status = Core::INITIALIZING;

   core_id_t core_id = m_scheduler->threadCreate(thread_id);
   if (core_id != INVALID_CORE_ID)
   {
      Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
      thread->setCore(core);
      core->setState(Core::INITIALIZING);
   }

   return thread;
}

void ThreadManager::onThreadStart(thread_id_t thread_id, SubsecondTime time)
{
   ScopedLock sl(m_thread_lock);
   LOG_PRINT("onThreadStart(%i)", thread_id);

   Thread *thread = getThreadFromID(thread_id);

   m_thread_tls->set(thread);
   thread->updateCoreTLS();

   HooksManager::ThreadTime args = { thread_id: thread_id, time: time };
   Sim()->getHooksManager()->callHooks(HookType::HOOK_THREAD_START, (void*)&args);

   Core *core = thread->getCore();
   if (core)
   {
      // Set the CoreState to 'RUNNING'
      core->setState(Core::RUNNING);

      PerformanceModel *pm = core->getPerformanceModel();
      pm->resetElapsedTime();
      pm->queueDynamicInstruction(new SpawnInstruction(time));

      LOG_PRINT("Setting status[%i] -> RUNNING", thread_id);
      m_thread_state[thread_id].status = Core::RUNNING;

      HooksManager::ThreadMigrate args = { thread_id: thread_id, core_id: core->getId(), time: time };
      Sim()->getHooksManager()->callHooks(HookType::HOOK_THREAD_MIGRATE, (void*)&args);
   }
   else
      m_thread_state[thread_id].status = Core::STALLED;

   if (m_thread_state[thread_id].waiter != INVALID_THREAD_ID)
   {
      getThreadFromID(m_thread_state[thread_id].waiter)->signal(time);
      m_thread_state[thread_id].waiter = INVALID_THREAD_ID;
   }
}

void ThreadManager::onThreadExit(thread_id_t thread_id)
{
   ScopedLock sl(m_thread_lock);

   Thread *thread = getThreadFromID(thread_id);
   CoreManager *core_manager = Sim()->getCoreManager();
   SubsecondTime time = core_manager->getCurrentCore()->getPerformanceModel()->getElapsedTime();

   LOG_ASSERT_ERROR((UInt32)thread_id < m_thread_state.size(), "Thread id out of range: %d", thread_id);

   assert(m_thread_state[thread_id].status == Core::RUNNING);
   m_thread_state[thread_id].status = Core::IDLE;

   HooksManager::ThreadTime args = { thread_id: thread_id, time: time };
   Sim()->getHooksManager()->callHooks(HookType::HOOK_THREAD_EXIT, (void*)&args);
   if (Sim()->getClockSkewMinimizationServer())
      Sim()->getClockSkewMinimizationServer()->signal();

   wakeUpWaiter(thread_id, time);

   // Set the CoreState to 'IDLE'
   core_manager->getCurrentCore()->setState(Core::IDLE);

   m_thread_tls->set(NULL);
   thread->setCore(NULL);
   thread->updateCoreTLS();
}

SInt32 ThreadManager::spawnThread(thread_id_t thread_id, thread_func_t func, void *arg)
{
   ScopedLock sl(getLock());

   SubsecondTime time_start = SubsecondTime::Zero();
   if (thread_id != INVALID_THREAD_ID)
   {
      Thread *thread = getThreadFromID(thread_id);
      Core *core = thread->getCore();
      time_start = core->getPerformanceModel()->getElapsedTime();
   }

   LOG_PRINT("(1) spawnThread with func: %p and arg: %p", func, arg);

   Thread *new_thread = createThread_unlocked();

   // Insert the request in the thread request queue
   ThreadSpawnRequest req = { thread_id, new_thread->getId(), time_start };
   m_thread_spawn_list.push(req);

   LOG_PRINT("Done with (2)");

   return new_thread->getId();
}

thread_id_t ThreadManager::getThreadToSpawn(SubsecondTime &time)
{
   ScopedLock sl(getLock());

   ThreadSpawnRequest req = m_thread_spawn_list.front();
   m_thread_spawn_list.pop();

   time = req.time;
   return req.thread_id;
}

void ThreadManager::waitForThreadStart(thread_id_t thread_id, thread_id_t wait_thread_id)
{
   ScopedLock sl(getLock());
   Thread *self = getThreadFromID(thread_id);

   if (m_thread_state[wait_thread_id].status == Core::INITIALIZING)
   {
      LOG_ASSERT_ERROR(m_thread_state[wait_thread_id].waiter == INVALID_THREAD_ID,
                       "Multiple threads waiting for thread: %d", wait_thread_id);

      m_thread_state[wait_thread_id].waiter = thread_id;
      self->wait(getLock());
   }
}

bool ThreadManager::areAllCoresRunning()
{
   // Check if all the cores are running
   bool is_all_running = true;
   for (SInt32 i = 0; i < (SInt32) m_thread_state.size(); i++)
   {
      if (m_thread_state[i].status == Core::IDLE)
      {
         is_all_running = false;
         break;
      }
   }

   return is_all_running;
}

void ThreadManager::joinThread(thread_id_t thread_id, thread_id_t join_thread_id)
{
   Thread *thread = getThreadFromID(thread_id);
   Core *core = thread->getCore();
   SubsecondTime end_time;

   LOG_PRINT("Joining on thread: %d", join_thread_id);

   {
      ScopedLock sl(getLock());

      if (m_thread_state[join_thread_id].status == Core::IDLE)
      {
         LOG_PRINT("Not running.");
         return;
      }

      SubsecondTime start_time = core->getPerformanceModel()->getElapsedTime();

      LOG_ASSERT_ERROR(m_thread_state[join_thread_id].waiter == INVALID_THREAD_ID,
                       "Multiple threads joining on thread: %d", join_thread_id);

      m_thread_state[join_thread_id].waiter = thread_id;
      end_time = stallThread(thread_id, ThreadManager::STALL_JOIN, start_time);
   }

   if (thread->reschedule(end_time, core))
      core = thread->getCore();

   core->getPerformanceModel()->queueDynamicInstruction(new SyncInstruction(end_time, SyncInstruction::JOIN));

   LOG_PRINT("Exiting join thread.");
}

void ThreadManager::wakeUpWaiter(thread_id_t thread_id, SubsecondTime time)
{
   if (m_thread_state[thread_id].waiter != INVALID_THREAD_ID)
   {
      LOG_PRINT("Waking up core: %d at time: %s", m_thread_state[thread_id].waiter, itostr(time).c_str());

      // Resume the 'pthread_join' caller
      resumeThread(m_thread_state[thread_id].waiter, thread_id, time);

      m_thread_state[thread_id].waiter = INVALID_THREAD_ID;
   }
   LOG_PRINT("Exiting wakeUpWaiter");
}

void ThreadManager::stallThread_async(thread_id_t thread_id, stall_type_t reason, SubsecondTime time)
{
   LOG_PRINT("Core(%i) -> STALLED", thread_id);
   m_thread_state[thread_id].status = Core::STALLED;

   HooksManager::ThreadStall args = { thread_id: thread_id, reason: reason, time: time };
   Sim()->getHooksManager()->callHooks(HookType::HOOK_THREAD_STALL, (void*)&args);
   if (Sim()->getClockSkewMinimizationServer())
      Sim()->getClockSkewMinimizationServer()->signal();
}

SubsecondTime ThreadManager::stallThread(thread_id_t thread_id, stall_type_t reason, SubsecondTime time)
{
   stallThread_async(thread_id, reason, time);
   return getThreadFromID(thread_id)->wait(m_thread_lock);
}

void ThreadManager::resumeThread(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time, void *msg)
{
   // We still have the m_thread_lock, so thread doesn't actually start running again until caller releases this lock
   getThreadFromID(thread_id)->signal(time, msg);

   LOG_PRINT("Core(%i) -> RUNNING", thread_id);
   m_thread_state[thread_id].status = Core::RUNNING;

   HooksManager::ThreadResume args = { thread_id: thread_id, thread_by: thread_by, time: time };
   Sim()->getHooksManager()->callHooks(HookType::HOOK_THREAD_RESUME, (void*)&args);
}

bool ThreadManager::isThreadRunning(thread_id_t thread_id)
{
   return (m_thread_state[thread_id].status == Core::RUNNING);
}

bool ThreadManager::isThreadInitializing(thread_id_t thread_id)
{
   return (m_thread_state[thread_id].status == Core::INITIALIZING);
}
