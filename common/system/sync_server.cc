#include "sync_server.h"
#include "sync_client.h"
#include "simulator.h"
#include "thread_manager.h"
#include "subsecond_time.h"
#include "config.hpp"

// -- SimMutex -- //

SimMutex::SimMutex()
      : m_owner(NO_OWNER)
{ }

SimMutex::~SimMutex()
{
   #if 0 // Disabled: applications are not required to do proper cleanup
   if (! m_waiting.empty()) {
      printf("WARNING: Waiters remaining for SimMutex@%p: ", this);
      while(! m_waiting.empty()) {
         printf("%d ", m_waiting.front());
         m_waiting.pop();
      }
      printf("\n");
   }
   #endif
}

bool SimMutex::isLocked(thread_id_t thread_id)
{
   if (m_owner == NO_OWNER)
      return false;
   else if (m_owner == thread_id)
      return false;
   else
      return true;
}

SubsecondTime SimMutex::lock(thread_id_t thread_id, SubsecondTime time)
{
   if (m_owner == NO_OWNER)
   {
      m_owner = thread_id;
      return time;
   }
   else
   {
      m_waiting.push(thread_id);
      return Sim()->getThreadManager()->stallThread(thread_id, ThreadManager::STALL_MUTEX, time);
   }
}

bool SimMutex::lock_async(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time)
{
   if (m_owner == NO_OWNER)
   {
      m_owner = thread_id;
      Sim()->getThreadManager()->resumeThread(m_owner, thread_by, time);
      return true;
   }
   else
   {
      m_waiting.push(thread_id);
      return false;
   }
}

thread_id_t SimMutex::unlock(thread_id_t thread_id, SubsecondTime time)
{
   assert(m_owner == thread_id);
   if (m_waiting.empty())
   {
      m_owner = NO_OWNER;
   }
   else
   {
      thread_id_t waiter = m_waiting.front();
      m_waiting.pop();
      m_owner = waiter;
      Sim()->getThreadManager()->resumeThread(m_owner, thread_id, time);
   }
   return m_owner;
}

// -- SimCond -- //
SimCond::SimCond() {}
SimCond::~SimCond()
{
   #if 0 // Disabled: applications are not required to do proper cleanup
   if (!m_waiting.empty()) {
      printf("Threads still waiting for SimCond@%p: ", this);
      while(!m_waiting.empty()) {
         printf("%u ", m_waiting.back().m_thread_id);
         m_waiting.pop();
      }
      printf("\n");
   }
   #endif
}

SubsecondTime SimCond::wait(thread_id_t thread_id, SubsecondTime time, SimMutex * simMux)
{
   simMux->unlock(thread_id, time);

   m_waiting.push(CondWaiter(thread_id, simMux));

   return Sim()->getThreadManager()->stallThread(thread_id, ThreadManager::STALL_COND, time);
}

thread_id_t SimCond::signal(thread_id_t thread_id, SubsecondTime time)
{
   // If there is a list of threads waiting, wake up one of them
   if (!m_waiting.empty())
   {
      CondWaiter woken = m_waiting.front();
      m_waiting.pop();

      if (woken.m_mutex->lock_async(woken.m_thread_id, thread_id, time))
      {
         // Woken up thread is able to grab lock immediately
         return woken.m_thread_id;
      }
      else
      {
         // Woken up thread is *NOT* able to grab lock immediately
         return INVALID_THREAD_ID;
      }
   }

   // There are *NO* threads waiting on the condition variable
   return INVALID_THREAD_ID;
}

void SimCond::broadcast(thread_id_t thread_id, SubsecondTime time)
{
   while(!m_waiting.empty())
   {
      CondWaiter woken = m_waiting.front();
      m_waiting.pop();
      woken.m_mutex->lock_async(woken.m_thread_id, thread_id, time);
   }
}

// -- SimBarrier -- //
SimBarrier::SimBarrier(UInt32 count)
      : m_count(count)
{
}

SimBarrier::~SimBarrier()
{
   if (!m_waiting.empty()) {
      printf("Threads still waiting for SimBarrier@%p: ", this);
      while(!m_waiting.empty()) {
         printf("%u ", m_waiting.back());
         m_waiting.pop();
      }
      printf("\n");
   }
}

SubsecondTime SimBarrier::wait(thread_id_t thread_id, SubsecondTime time)
{
   // We are the last thread to reach the barrier
   if (m_waiting.size() == m_count - 1)
   {
      while(! m_waiting.empty())
      {
         // Resuming all the threads stalled at the barrier

         thread_id_t waiter = m_waiting.front();
         m_waiting.pop();
         Sim()->getThreadManager()->resumeThread(waiter, thread_id, time);
      }
      return time;
   }
   else
   {
      m_waiting.push(thread_id);
      return Sim()->getThreadManager()->stallThread(thread_id, ThreadManager::STALL_BARRIER, time);
   }
}

// -- SyncServer -- //

SyncServer::SyncServer()
{
   m_reschedule_cost = SubsecondTime::NS() * Sim()->getCfg()->getInt("perf_model/sync/reschedule_cost");
}

SyncServer::~SyncServer()
{ }

SimMutex * SyncServer::getMutex(carbon_mutex_t *mux, bool canCreate)
{
   // if mux is the address of a pthread mutex (with default initialization, not through pthread_mutex_init),
   // look it up in m_mutexes or create a new one if it's the first time we see it
   if (m_mutexes.count(mux))
      return &m_mutexes[mux];
   else if (canCreate)
   {
      m_mutexes[mux] = SimMutex();
      return &m_mutexes[mux];
   }
   else
   {
      LOG_ASSERT_ERROR(false, "Invalid mutex id passed");
      return NULL;
   }
}

void SyncServer::mutexInit(thread_id_t thread_id, carbon_mutex_t *mux)
{
   ScopedLock sl(Sim()->getThreadManager()->getLock());
   getMutex(mux);
}

std::pair<SubsecondTime, bool> SyncServer::mutexLock(thread_id_t thread_id, carbon_mutex_t *mux, bool tryLock, SubsecondTime time)
{
   ScopedLock sl(Sim()->getThreadManager()->getLock());
   SimMutex *psimmux = getMutex(mux);

   if (tryLock && psimmux->isLocked(thread_id))
   {
      // notify the owner of failure
      return std::make_pair(time, false);
   }
   else
   {
      SubsecondTime time_end = psimmux->lock(thread_id, time);
      return std::make_pair(time_end + m_reschedule_cost, true);
   }
}

SubsecondTime SyncServer::mutexUnlock(thread_id_t thread_id, carbon_mutex_t *mux, SubsecondTime time)
{
   ScopedLock sl(Sim()->getThreadManager()->getLock());
   SimMutex *psimmux = getMutex(mux, false);

   thread_id_t new_owner = psimmux->unlock(thread_id, time + m_reschedule_cost);

   SubsecondTime new_time = time + (new_owner == SimMutex::NO_OWNER ? SubsecondTime::Zero() : m_reschedule_cost /* we had to call futex_wake */);
   return new_time;
}

// -- Condition Variable Stuffs -- //
SimCond * SyncServer::getCond(carbon_cond_t *cond, bool canCreate)
{
   // if cond is the address of a pthread cond (with default initialization, not through pthread_cond_init),
   // look it up in m_conds or create a new one if it's the first time we see it
   if (m_conds.count(cond))
      return &m_conds[cond];
   else if (canCreate)
   {
      m_conds[cond] = SimCond();
      return &m_conds[cond];
   }
   else
   {
      LOG_ASSERT_ERROR(false, "Invalid cond id passed");
      return NULL;
   }
}

void SyncServer::condInit(thread_id_t thread_id, carbon_cond_t *cond)
{
   ScopedLock sl(Sim()->getThreadManager()->getLock());
   getCond(cond);
}

SubsecondTime SyncServer::condWait(thread_id_t thread_id, carbon_cond_t *cond, carbon_mutex_t *mux, SubsecondTime time)
{
   ScopedLock sl(Sim()->getThreadManager()->getLock());
   SimMutex *psimmux = getMutex(mux);
   SimCond *psimcond = getCond(cond);

   return psimcond->wait(thread_id, time, psimmux);
}

SubsecondTime SyncServer::condSignal(thread_id_t thread_id, carbon_cond_t *cond, SubsecondTime time)
{
   ScopedLock sl(Sim()->getThreadManager()->getLock());
   SimCond *psimcond = getCond(cond);

   psimcond->signal(thread_id, time);

   return time;
}

SubsecondTime SyncServer::condBroadcast(thread_id_t thread_id, carbon_cond_t *cond, SubsecondTime time)
{
   ScopedLock sl(Sim()->getThreadManager()->getLock());
   SimCond *psimcond = getCond(cond);

   psimcond->broadcast(thread_id, time);

   return time;
}

void SyncServer::barrierInit(thread_id_t thread_id, carbon_barrier_t *barrier, UInt32 count)
{
   ScopedLock sl(Sim()->getThreadManager()->getLock());

   m_barriers.push_back(SimBarrier(count));
   *barrier = (carbon_barrier_t)m_barriers.size()-1;
}

SubsecondTime SyncServer::barrierWait(thread_id_t thread_id, carbon_barrier_t *barrier, SubsecondTime time)
{
   ScopedLock sl(Sim()->getThreadManager()->getLock());
   SimBarrier *psimbarrier = &m_barriers[*barrier];

   return psimbarrier->wait(thread_id, time);
}
