/*
 * This file is covered under the Interval Academic License, see LICENCE.interval
 */

#include "smt_timer.h"
#include "simulator.h"
#include "thread.h"
#include "circular_log.h"

SmtTimer::SmtThread::SmtThread(Core *core, PerformanceModel *perf)
      : core(core)
      , perf(perf)
      , thread(NULL)
      , in_wakeup(false)
      , running(false)
      , in_barrier(false)
{
}

SmtTimer::SmtThread::~SmtThread()
{
}

SmtTimer::SmtTimer(uint64_t num_threads)
   : m_num_threads(num_threads)
   , in_sync(false)
   , enabled(true)
   , execute_thread(0)
{
   Sim()->getHooksManager()->registerHook(HookType::HOOK_ROI_BEGIN, SmtTimer::hookRoiBegin, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_START, SmtTimer::hookThreadStart, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_EXIT, SmtTimer::hookThreadExit, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_STALL, SmtTimer::hookThreadStall, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_RESUME, SmtTimer::hookThreadResume, (UInt64)this);
   Sim()->getHooksManager()->registerHook(HookType::HOOK_THREAD_MIGRATE, SmtTimer::hookThreadMigrate, (UInt64)this);
}

SmtTimer::~SmtTimer()
{
   for(std::vector<SmtThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
      delete *it;
}

UInt8 SmtTimer::registerThread(Core *core, PerformanceModel *perf)
{
   smtthread_id_t thread_num = m_threads.size();

   SmtThread *thread = new SmtThread(core, perf);
   m_threads.push_back(thread);

   initializeThread(thread_num);

   return thread_num;
}

SmtTimer::smtthread_id_t SmtTimer::findSmtThreadFromThread(thread_id_t thread_id)
{
   if (thread_id == INVALID_THREAD_ID)
      return INVALID_SMTTHREAD_ID;
   for(smtthread_id_t smtthread_id = 0; smtthread_id < m_threads.size(); ++smtthread_id)
      if (m_threads[smtthread_id]->thread && (m_threads[smtthread_id]->thread->getId() == thread_id))
         return smtthread_id;
   return INVALID_SMTTHREAD_ID;
}

SmtTimer::smtthread_id_t SmtTimer::findSmtThreadFromCore(core_id_t core_id)
{
   if (core_id == INVALID_CORE_ID)
      return INVALID_SMTTHREAD_ID;
   for(smtthread_id_t smtthread_id = 0; smtthread_id < m_threads.size(); ++smtthread_id)
      if (m_threads[smtthread_id]->core->getId() == core_id)
         return smtthread_id;
   return INVALID_SMTTHREAD_ID;
}

bool SmtTimer::isBarrierReached()
{
   // A thread has to give up m_lock when calling the global barrier
   // When this happens, we don't want anyone to continue
   if (in_sync)
      return false;

   bool any_in_barrier = false;
   for(smtthread_id_t thread_num = 0; thread_num < m_threads.size(); ++thread_num)
   {
      SmtThread *thread = m_threads[thread_num];

      if (thread->running && !thread->in_barrier)
      {
         return false;
      }
      else if (thread->in_barrier)
      {
         any_in_barrier = true;
      }
   }

   return any_in_barrier;
}

bool SmtTimer::barrierRelease(bool release_all, smtthread_id_t thread_id)
{
   bool release_me = false;
   execute_thread = thread_id;

   // Release those threads for which we want more instructions

   unsigned int num_running = 0;
   for(smtthread_id_t thread_num = 0; thread_num < m_threads.size(); ++thread_num)
   {
      SmtThread *thread = m_threads[thread_num];

      if (!thread->in_barrier)
      {
         if (thread->running)
            ++num_running;

         // Thread wasn't stalled, no need to wake it up
         continue;
      }

      if (threadHasEnoughInstructions(thread_num))
      {
         // We have enough instructions for now, keep the thread stalled
         continue;
      }

      if (thread->running)
         ++num_running;

      // We want more instructions from this thread, release it
      if (thread_num == thread_id)
      {
         release_me = true;
      }
      else
      {
         thread->in_barrier = false;
         thread->cond.signal();
      }

      if (!release_all)
      {
         execute_thread = thread_num;
         break;
      }
   }


   UInt64 limit = 1;

   while (num_running == 0)
   {
      // All threads have enough instructions. We still need to release someone to make forward progress.
      // Find the thread with the (approximately) lowest number of surplus instructions and release it

      for(smtthread_id_t thread_num = 0; thread_num < m_threads.size(); ++thread_num)
      {
         SmtThread *thread = m_threads[thread_num];

         if (thread->in_barrier && thread->running && threadNumSurplusInstructions(thread_num) < limit)
         {
            ++num_running;

            if (thread_num == thread_id)
            {
               release_me = true;
            }
            else
            {
               thread->in_barrier = false;
               thread->cond.signal();
            }

            if (!release_all)
            {
               execute_thread = thread_num;
               break;
            }
         }
      }

      limit <<= 1;
   }

   return release_me;
}


bool SmtTimer::barrier(smtthread_id_t thread_id)
{
   if (!enabled)
      return false;

   SmtThread *smtthread = m_threads[thread_id];
   const Thread *thread = smtthread->thread;
   LOG_ASSERT_ERROR(thread != NULL, "How can SMT barrier be called when there is not thread running?");

   smtthread->in_barrier = true;
   CLOG("smtbarrier", "Entry core %d thread %d", smtthread->core->getId(), thread->getId());

   if (!isBarrierReached())
   {
      smtthread->cond.wait(m_lock);
   }
   else
   {
      bool release_me = barrierRelease(false, thread_id);

      if (release_me)
         smtthread->in_barrier = false;
      else
         smtthread->cond.wait(m_lock);
   }

   CLOG("smtbarrier", "Exit core %d thread %d (execute = %s)",
     smtthread->core->getId(), thread->getId(), thread_id == execute_thread ? "true" : "false");

   // Will we call execute() ?
   return thread_id == execute_thread;
}


void SmtTimer::signalBarrier()
{
   if (isBarrierReached())
      barrierRelease(false, INVALID_SMTTHREAD_ID);
}

void SmtTimer::roiBegin()
{
   SubsecondTime time = Sim()->getClockSkewMinimizationServer()->getGlobalTime();
   // Don't wait for any Sync/SpawnInstructions that were announced a while ago,
   // they were ignored because performance models were disabled.
   for(smtthread_id_t smtthread_id = 0; smtthread_id < m_threads.size(); ++smtthread_id)
   {
      m_threads[smtthread_id]->in_wakeup = false;
      synchronize(smtthread_id, time);
   }
}

void SmtTimer::threadStart(HooksManager::ThreadTime *argument)
{
   ScopedLock sl(m_lock);

   smtthread_id_t smtthread_id = findSmtThreadFromThread(argument->thread_id);
   if (smtthread_id != INVALID_SMTTHREAD_ID)
   {
      m_threads[smtthread_id]->running = true;
      notifyNumActiveThreadsChange();
   }
}

void SmtTimer::threadExit(HooksManager::ThreadTime *argument)
{
   ScopedLock sl(m_lock);

   smtthread_id_t smtthread_id = findSmtThreadFromThread(argument->thread_id);
   if (smtthread_id != INVALID_SMTTHREAD_ID)
   {
      m_threads[smtthread_id]->running = false;
      notifyNumActiveThreadsChange();
   }
   // Re-evaluate whether the stalled thread can run (maybe it was waiting for us?)
   // Note: HOOK_THREAD_EXIT happens right at the end, after possible rescheduling.
   // So always call signalBarrier() even though the thread is not currently here, since it may have been before
   signalBarrier();
}

void SmtTimer::threadStall(HooksManager::ThreadStall *argument)
{
   ScopedLock sl(m_lock);

   smtthread_id_t smtthread_id = findSmtThreadFromThread(argument->thread_id);
   if (smtthread_id != INVALID_SMTTHREAD_ID)
   {
      m_threads[smtthread_id]->running = false;
      notifyNumActiveThreadsChange();
   }
   // Re-evaluate whether the stalled thread can run (maybe it was waiting for us?)
   // Note: HOOK_THREAD_STALL happens right at the end, after possible rescheduling.
   // So always call signalBarrier() even though the thread is not currently here, since it may have been before
   signalBarrier();
}

void SmtTimer::threadResume(HooksManager::ThreadResume *argument)
{
   ScopedLock sl(m_lock);

   // When a thread wakes up, it will usually call simulate() before it processes the SYNC_INSTRUCTION
   // that updates their thread->now to the wakeup time. In this intervening period,
   // we should not return any latency from simulate() since that will be huge (this->now has advanced,
   // because the other threads have ran, leaving this thread who was asleep behind; but thread->now
   // is still set to the time the thread went to sleep).
   // Instead, we listen for HookType::HOOK_THREAD_RESUME (called from the MCP, before the thread gets its
   // wakeup message and way before the offending simulate() call) and set thread->in_wakeup, which signals
   // simulate() to not return any latency until RobSmtTimer::synchronize() was called which updates
   // thread->now to the correct wakeup time.

   smtthread_id_t smtthread_id = findSmtThreadFromThread(argument->thread_id);
   if (smtthread_id != INVALID_SMTTHREAD_ID)
   {
      m_threads[smtthread_id]->in_wakeup = true;
      m_threads[smtthread_id]->running = true;
      notifyNumActiveThreadsChange();
   }
}

void SmtTimer::threadMigrate(HooksManager::ThreadMigrate *argument)
{
   ScopedLock sl(m_lock);

   // Same as in wakeup case: thread will send a SpawnInstruction, but we don't want to send huge latencies
   // to simulate() calls that precede it

   // If this thread was previously running on this core, mark it as gone
   smtthread_id_t smtthread_id = findSmtThreadFromThread(argument->thread_id);
   if (smtthread_id != INVALID_SMTTHREAD_ID)
   {
      SmtThread *thread = m_threads[smtthread_id];
      thread->running = false;
      thread->thread = NULL;
      notifyNumActiveThreadsChange();
      if (thread->in_barrier)
      {
         thread->in_barrier = false;
         thread->cond.signal();
      }
   }

   // If the new core is one of ours, mark it as running this thread
   smtthread_id = findSmtThreadFromCore(argument->core_id);
   if (smtthread_id != INVALID_SMTTHREAD_ID)
   {
      #ifdef DEBUG_PERCYCLE
         std::cout<<"** ["<<argument->thread_id<<"] threadMigrate"<<std::endl;
      #endif
      m_threads[smtthread_id]->in_wakeup = true;
      m_threads[smtthread_id]->running = true;
      m_threads[smtthread_id]->thread = Sim()->getThreadManager()->getThreadFromID(argument->thread_id);
      notifyNumActiveThreadsChange();
   }
}

void SmtTimer::simulate(smtthread_id_t thread_id)
{
   SmtThread *thread = m_threads[thread_id];

   if (threadNumSurplusInstructions(thread_id) > 128)
   {
      if (barrier(thread_id))
      {
         execute();

         // We executed this cycle, make sure no-one else (potentially released through
         // sync->periodic->reschedule->migrate) executes again.
         execute_thread = INVALID_THREAD_ID;

         ClockSkewMinimizationClient *client = thread->core->getClockSkewMinimizationClient();
         if (client)
         {
            in_sync = true;
            m_lock.release();
            client->synchronize();
            m_lock.acquire();
            in_sync = false;
         }

         barrierRelease(true, thread_id);
      }
   }
}

void SmtTimer::enable()
{
   enabled = true;
}

void SmtTimer::disable()
{
   enabled = false;

   for(smtthread_id_t thread_num = 0; thread_num < m_threads.size(); ++thread_num)
   {
      SmtThread *thread = m_threads[thread_num];

      if (thread->in_barrier)
      {
         thread->in_barrier = false;
         thread->cond.signal();
      }
   }
}

char SmtTimer::getStateStr(smtthread_id_t thread_num)
{
   SmtThread *thread = m_threads[thread_num];
   if (thread->running)
   {
      if (thread->in_barrier)
         return 'B';
      else
         return 'R';
   }
   else
   {
      if (thread->in_barrier)
         return 'b';
      else
         return '_';
   }
}
