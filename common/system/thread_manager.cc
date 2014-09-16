#include "thread_manager.h"
#include "core_manager.h"
#include "performance_model.h"
#include "instruction.h"
#include "hooks_manager.h"
#include "config.h"
#include "log.h"
#include "stats.h"
#include "transport.h"
#include "simulator.h"
#include "clock_skew_minimization_object.h"
#include "core.h"
#include "thread.h"
#include "scheduler.h"
#include "syscall_server.h"
#include "circular_log.h"

#include <sys/syscall.h>
#include "os_compat.h"

const char* ThreadManager::stall_type_names[] = {
   "unscheduled", "broken", "join", "mutex", "cond", "barrier", "futex", "pause", "sleep", "syscall"
};
static_assert(ThreadManager::STALL_TYPES_MAX == sizeof(ThreadManager::stall_type_names) / sizeof(char*),
              "Not enough values in ThreadManager::stall_type_names");

ThreadManager::ThreadManager()
   : m_thread_tls(TLS::create())
   , m_scheduler(Scheduler::create(this))
{
}

ThreadManager::~ThreadManager()
{
   for (UInt32 i = 0; i < m_thread_state.size(); i++)
   {
      #if 0 // Disabled: applications are not required to do proper cleanup
      if (m_thread_state[i].status != Core::IDLE)
         fprintf(stderr, "Thread %d still active when ThreadManager destructs\n", i);
      #endif
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

Thread* ThreadManager::findThreadByTid(pid_t tid)
{
   for (UInt32 thread_id = 0; thread_id < m_threads.size(); ++thread_id)
   {
      if (m_threads.at(thread_id)->m_os_info.tid == tid)
         return m_threads.at(thread_id);
   }
   return NULL;
}

Thread* ThreadManager::createThread(app_id_t app_id, thread_id_t creator_thread_id)
{
   ScopedLock sl(m_thread_lock);
   return createThread_unlocked(app_id, creator_thread_id);
}

Thread* ThreadManager::createThread_unlocked(app_id_t app_id, thread_id_t creator_thread_id)
{
   thread_id_t thread_id = m_threads.size();
   Thread *thread = new Thread(thread_id, app_id);
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

   Sim()->getStatsManager()->logEvent(StatsManager::EVENT_THREAD_CREATE, SubsecondTime::MaxTime(), core_id, thread_id, app_id, creator_thread_id, "");

   HooksManager::ThreadCreate args = { thread_id: thread_id, creator_thread_id: creator_thread_id };
   Sim()->getHooksManager()->callHooks(HookType::HOOK_THREAD_CREATE, (UInt64)&args);
   CLOG("thread", "Create %d", thread_id);

   return thread;
}

void ThreadManager::onThreadStart(thread_id_t thread_id, SubsecondTime time)
{
   ScopedLock sl(m_thread_lock);
   LOG_PRINT("onThreadStart(%i)", thread_id);

   Thread *thread = getThreadFromID(thread_id);

   m_thread_tls->set(thread);
   thread->updateCoreTLS();

   // Set thread state to running for the duration of HOOK_THREAD_START, we'll move it to stalled later on if it didn't have a core
   m_thread_state[thread_id].status = Core::RUNNING;

   HooksManager::ThreadTime args = { thread_id: thread_id, time: time };
   Sim()->getHooksManager()->callHooks(HookType::HOOK_THREAD_START, (UInt64)&args);
   // Note: we may have been rescheduled during HOOK_THREAD_START
   // (Happens if core was occupied during our createThread() but became free since then)
   CLOG("thread", "Start %d", thread_id);

   Core *core = thread->getCore();
   if (core)
   {
      // Set the CoreState to 'RUNNING'
      core->setState(Core::RUNNING);

      PerformanceModel *pm = core->getPerformanceModel();
      // If the core already has a later time, we have to wait
      time = std::max(time, pm->getElapsedTime());
      pm->queuePseudoInstruction(new SpawnInstruction(time));

      LOG_PRINT("Setting status[%i] -> RUNNING", thread_id);
      m_thread_state[thread_id].status = Core::RUNNING;

      HooksManager::ThreadMigrate args = { thread_id: thread_id, core_id: core->getId(), time: time };
      Sim()->getHooksManager()->callHooks(HookType::HOOK_THREAD_MIGRATE, (UInt64)&args);
   }
   else
   {
      m_thread_state[thread_id].status = Core::STALLED;
      m_thread_state[thread_id].stalled_reason = STALL_UNSCHEDULED;
   }

   if (m_thread_state[thread_id].waiter != INVALID_THREAD_ID)
   {
      getThreadFromID(m_thread_state[thread_id].waiter)->signal(time);
      m_thread_state[thread_id].waiter = INVALID_THREAD_ID;
   }
}

void ThreadManager::onThreadExit(thread_id_t thread_id)
{
   ScopedLock sl(m_thread_lock);

   LOG_ASSERT_ERROR((UInt32)thread_id < m_thread_state.size(), "Thread id out of range: %d", thread_id);

   Thread *thread = getThreadFromID(thread_id);
   Core *core = thread->getCore();
   LOG_ASSERT_ERROR(core != NULL, "Thread ended while not running on a core?");

   SubsecondTime time = core->getPerformanceModel()->getElapsedTime();

   assert(m_thread_state[thread_id].status == Core::RUNNING);
   m_thread_state[thread_id].status = Core::IDLE;

   // Implement pthread_join
   wakeUpWaiter(thread_id, time);

   // Implement CLONE_CHILD_CLEARTID
   if (thread->m_os_info.clear_tid)
   {
      uint32_t zero = 0;
      core->accessMemory(Core::NONE, Core::WRITE, thread->m_os_info.tid_ptr, (char*)&zero, sizeof(zero));

      SubsecondTime end_time; // ignored
      Sim()->getSyscallServer()->futexWake(thread_id, (int*)thread->m_os_info.tid_ptr, 1, FUTEX_BITSET_MATCH_ANY, time, end_time);
   }

   // Set the CoreState to 'IDLE'
   core->setState(Core::IDLE);

   m_thread_tls->set(NULL);
   thread->setCore(NULL);
   thread->updateCoreTLS();

   Sim()->getStatsManager()->logEvent(StatsManager::EVENT_THREAD_EXIT, SubsecondTime::MaxTime(), core->getId(), thread_id, 0, 0, "");

   HooksManager::ThreadTime args = { thread_id: thread_id, time: time };
   Sim()->getHooksManager()->callHooks(HookType::HOOK_THREAD_EXIT, (UInt64)&args);
   CLOG("thread", "Exit %d", thread_id);
}

thread_id_t ThreadManager::spawnThread(thread_id_t thread_id, app_id_t app_id)
{
   ScopedLock sl(getLock());

   SubsecondTime time_start = SubsecondTime::Zero();
   if (thread_id != INVALID_THREAD_ID)
   {
      Thread *thread = getThreadFromID(thread_id);
      Core *core = thread->getCore();
      time_start = core->getPerformanceModel()->getElapsedTime();
   }

   Thread *new_thread = createThread_unlocked(app_id, thread_id);

   // Insert the request in the thread request queue
   ThreadSpawnRequest req = { thread_id, new_thread->getId(), time_start };
   m_thread_spawn_list.push(req);

   LOG_PRINT("Done with (2)");

   return new_thread->getId();
}

thread_id_t ThreadManager::getThreadToSpawn(SubsecondTime &time)
{
   ScopedLock sl(getLock());

   LOG_ASSERT_ERROR(!m_thread_spawn_list.empty(), "Have no thread to spawn");

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

void ThreadManager::moveThread(thread_id_t thread_id, core_id_t core_id, SubsecondTime time)
{
   Thread *thread = getThreadFromID(thread_id);
   CLOG("thread", "Move %d from %d to %d", thread_id, thread->getCore() ? thread->getCore()->getId() : -1, core_id);

   if (Core *core = thread->getCore())
      core->setState(Core::IDLE);

   if (core_id == INVALID_CORE_ID)
   {
      thread->setCore(NULL);
   }
   else
   {
      if (thread->getCore() == NULL)
      {
         // Unless thread was stalled for sync/futex/..., wake it up
         if (
            m_thread_state[thread_id].status == Core::STALLED
            && m_thread_state[thread_id].stalled_reason == STALL_UNSCHEDULED
         )
            resumeThread(thread_id, INVALID_THREAD_ID, time);
      }

      Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
      thread->setCore(core);
      if (getThreadState(thread_id) != Core::STALLED)
         core->setState(Core::RUNNING);
   }

   HooksManager::ThreadMigrate args = { thread_id: thread_id, core_id: core_id, time: time };
   Sim()->getHooksManager()->callHooks(HookType::HOOK_THREAD_MIGRATE, (UInt64)&args);
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

   core->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(end_time, SyncInstruction::JOIN));

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
   m_thread_state[thread_id].stalled_reason = reason;

   HooksManager::ThreadStall args = { thread_id: thread_id, reason: reason, time: time };
   Sim()->getHooksManager()->callHooks(HookType::HOOK_THREAD_STALL, (UInt64)&args);
   CLOG("thread", "Stall %d (%s)", thread_id, ThreadManager::stall_type_names[reason]);
}

SubsecondTime ThreadManager::stallThread(thread_id_t thread_id, stall_type_t reason, SubsecondTime time)
{
   stallThread_async(thread_id, reason, time);
   // When all threads are stalled, we have a deadlock -- unless we let the barrier advance time
   // which may wake up threads that are sleeping or waiting on a futex with a timeout value.
   while(!anyThreadRunning())
   {
      Sim()->getClockSkewMinimizationServer()->advance();
   }
   // It's possible that a HOOK_PERIODIC, called by SkewMinServer::signal(), called by stallThread_async(), woke us up again.
   // We will then have been signal()d, but this signal was lost since we weren't in wait()
   // If this is the case, don't go to sleep but return our wakeup time immediately
   if (m_thread_state[thread_id].status == Core::RUNNING)
      return getThreadFromID(thread_id)->getWakeupTime();
   else
      return getThreadFromID(thread_id)->wait(m_thread_lock);
}

void ThreadManager::resumeThread_async(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time, void *msg)
{
   LOG_PRINT("Core(%i) -> RUNNING", thread_id);
   m_thread_state[thread_id].status = Core::RUNNING;

   HooksManager::ThreadResume args = { thread_id: thread_id, thread_by: thread_by, time: time };
   Sim()->getHooksManager()->callHooks(HookType::HOOK_THREAD_RESUME, (UInt64)&args);
   CLOG("thread", "Resume %d (by %d)", thread_id, thread_by);
}

void ThreadManager::resumeThread(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time, void *msg)
{
   // We still have the m_thread_lock, so thread doesn't actually start running again until caller releases this lock
   getThreadFromID(thread_id)->signal(time, msg);

   resumeThread_async(thread_id, thread_by, time, msg);
}

bool ThreadManager::isThreadRunning(thread_id_t thread_id)
{
   return (m_thread_state[thread_id].status == Core::RUNNING);
}

bool ThreadManager::isThreadInitializing(thread_id_t thread_id)
{
   return (m_thread_state[thread_id].status == Core::INITIALIZING);
}

bool ThreadManager::anyThreadRunning()
{
   for(thread_id_t thread_id = 0; thread_id < (thread_id_t)getNumThreads(); ++thread_id)
   {
      if (isThreadRunning(thread_id) || isThreadInitializing(thread_id))
         return true;
   }
   return false;
}
