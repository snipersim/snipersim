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
   if (! m_waiting.empty()) {
      printf("WARNING: Waiters remaining for SimMutex@%p: ", this);
      while(! m_waiting.empty()) {
         printf("%d ", m_waiting.front());
         m_waiting.pop();
      }
      printf("\n");
   }
}

bool SimMutex::isLocked(core_id_t core_id)
{
   if (m_owner == NO_OWNER)
      return false;
   else if (m_owner == core_id)
      return false;
   else
      return true;
}

bool SimMutex::lock(core_id_t core_id, SubsecondTime time)
{
   if (m_owner == NO_OWNER)
   {
      m_owner = core_id;
      return true;
   }
   else
   {
      Sim()->getThreadManager()->stallThread(core_id, time);
      m_waiting.push(core_id);
      return false;
   }
}

core_id_t SimMutex::unlock(core_id_t core_id, SubsecondTime time)
{
   assert(m_owner == core_id);
   if (m_waiting.empty())
   {
      m_owner = NO_OWNER;
   }
   else
   {
      // TODO FIXME: how does pthread_mutex handle this? Is it really the first requester that gets the lock next?
      m_owner =  m_waiting.front();
      m_waiting.pop();
      Sim()->getThreadManager()->resumeThread(m_owner, core_id, time);
   }
   return m_owner;
}

// -- SimCond -- //
// FIXME: Currently, 'simulated times' are ignored in the synchronization constructs
SimCond::SimCond() {}
SimCond::~SimCond()
{
   if (!m_waiting.empty()) {
      printf("Threads still waiting for SimCond@%p: ", this);
      while(!m_waiting.empty()) {
         printf("%u ", m_waiting.back().m_core_id);
         m_waiting.pop_back();
      }
      printf("\n");
   }
}

core_id_t SimCond::wait(core_id_t core_id, SubsecondTime time, SimMutex * simMux)
{
   Sim()->getThreadManager()->stallThread(core_id, time);

   // If we don't have any later signals, then put this request in the queue
   m_waiting.push_back(CondWaiter(core_id, simMux, time));
   return simMux->unlock(core_id, time);
}

core_id_t SimCond::signal(core_id_t core_id, SubsecondTime time)
{
   // If there is a list of threads waiting, wake up one of them
   if (!m_waiting.empty())
   {
      CondWaiter woken = *(m_waiting.begin());
      m_waiting.erase(m_waiting.begin());

      Sim()->getThreadManager()->resumeThread(woken.m_core_id, core_id, time);

      if (woken.m_mutex->lock(woken.m_core_id, time))
      {
         // Woken up thread is able to grab lock immediately
         return woken.m_core_id;
      }
      else
      {
         // Woken up thread is *NOT* able to grab lock immediately
         return INVALID_CORE_ID;
      }
   }

   // There are *NO* threads waiting on the condition variable
   return INVALID_CORE_ID;
}

void SimCond::broadcast(core_id_t core_id, SubsecondTime time, WakeupList &woken_list)
{
   for (ThreadQueue::iterator i = m_waiting.begin(); i != m_waiting.end(); i++)
   {
      CondWaiter woken = *(i);

      Sim()->getThreadManager()->resumeThread(woken.m_core_id, core_id, time);

      if (woken.m_mutex->lock(woken.m_core_id, time))
      {
         // Woken up thread is able to grab lock immediately
         woken_list.push_back(woken.m_core_id);
      }
   }

   // All waiting threads have been woken up from the CondVar queue
   m_waiting.clear();
}

// -- SimBarrier -- //
SimBarrier::SimBarrier(UInt32 count)
      : m_count(count)
      , m_max_time(SubsecondTime::Zero())
{
}

SimBarrier::~SimBarrier()
{
   if (!m_waiting.empty()) {
      printf("Threads still waiting for SimBarrier@%p: ", this);
      while(!m_waiting.empty()) {
         printf("%u ", m_waiting.back());
         m_waiting.pop_back();
      }
      printf("\n");
   }
}

void SimBarrier::wait(core_id_t core_id, SubsecondTime time, WakeupList &woken_list)
{
   m_waiting.push_back(core_id);

   Sim()->getThreadManager()->stallThread(core_id, time);

   assert(m_waiting.size() <= m_count);

   if (m_waiting.size() == 1)
      m_max_time = time;
   else if (time > m_max_time)
      m_max_time = time;

   // All threads have reached the barrier
   if (m_waiting.size() == m_count)
   {
      woken_list = m_waiting;

      for (WakeupList::iterator i = woken_list.begin(); i != woken_list.end(); i++)
      {
         // Resuming all the threads stalled at the barrier
         Sim()->getThreadManager()->resumeThread(*i, core_id, time);
      }
      m_waiting.clear();
   }
}

// -- SyncServer -- //

SyncServer::SyncServer(Network &network, UnstructuredBuffer &recv_buffer)
      : m_network(network),
      m_recv_buffer(recv_buffer)
{
   m_reschedule_cost = SubsecondTime::NS() * Sim()->getCfg()->getInt("perf_model/sync/reschedule_cost", 0);
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

void SyncServer::mutexInit(core_id_t core_id)
{
   carbon_mutex_t *mux;
   m_recv_buffer >> mux;

   getMutex(mux);

   m_network.netSend(core_id, MCP_RESPONSE_TYPE, (char*)&mux, sizeof(mux));
}

void SyncServer::mutexLock(core_id_t core_id)
{
   int tryLock;
   m_recv_buffer >> tryLock;

   carbon_mutex_t *mux;
   m_recv_buffer >> mux;

   SubsecondTime time;
   m_recv_buffer >> time;

   SimMutex *psimmux = getMutex(mux);

//printf("%u SyncServer::mutexLock mux(%x) try(%d) locked(%d)\n", core_id, mux, tryLock, psimmux->locked(core_id));
   if (tryLock && psimmux->isLocked(core_id))
   {
      // notify the owner of failure
      Reply r;
      r.dummy = SyncClient::MUTEX_TRYLOCK_RESPONSE;
      r.time = time;
      m_network.netSend(core_id, MCP_RESPONSE_TYPE, (char*)&r, sizeof(r));
   }
   else if (psimmux->lock(core_id, time))
   {
      // notify the owner
      Reply r;
      r.dummy = SyncClient::MUTEX_LOCK_RESPONSE;
      r.time = time;
      m_network.netSend(core_id, MCP_RESPONSE_TYPE, (char*)&r, sizeof(r));
   }
   else
   {
      // nothing...thread goes to sleep
   }
}

void SyncServer::mutexUnlock(core_id_t core_id)
{
   carbon_mutex_t *mux;
   m_recv_buffer >> mux;

   SubsecondTime time;
   m_recv_buffer >> time;

   SimMutex *psimmux = getMutex(mux, false);

//printf("%u SyncServer::mutexUnlock mux(%x)\n", core_id, mux);
   core_id_t new_owner = psimmux->unlock(core_id, time);

   if (new_owner != SimMutex::NO_OWNER)
   {
      // wake up the new owner
      Reply r;
      r.dummy = SyncClient::MUTEX_LOCK_RESPONSE;
      r.time = time + m_reschedule_cost;
      m_network.netSend(new_owner, MCP_RESPONSE_TYPE, (char*)&r, sizeof(r));
   }
   else
   {
      // nothing...
   }

   Reply r;
   r.dummy = SyncClient::MUTEX_UNLOCK_RESPONSE;
   SubsecondTime new_time = time + (new_owner == SimMutex::NO_OWNER ? SubsecondTime::Zero() : m_reschedule_cost /* we had to call futex_wake */);
   r.time = new_time;
   m_network.netSend(core_id, MCP_RESPONSE_TYPE, (char*)&r, sizeof(r));
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

void SyncServer::condInit(core_id_t core_id)
{
   carbon_cond_t *cond;
   m_recv_buffer >> cond;

   getCond(cond);

//printf("condInit = %d\n", cond);
   m_network.netSend(core_id, MCP_RESPONSE_TYPE, (char*)&cond, sizeof(cond));
}

void SyncServer::condWait(core_id_t core_id)
{
   carbon_cond_t *cond;
   carbon_mutex_t *mux;
   m_recv_buffer >> cond;
   m_recv_buffer >> mux;

   SubsecondTime time;
   m_recv_buffer >> time;

//printf("SyncServer::condWait mux(%x), cond(%x)\n", mux, cond);
   SimMutex *psimmux = getMutex(mux);
   SimCond *psimcond = getCond(cond);

   core_id_t new_mutex_owner = psimcond->wait(core_id, time, psimmux);

   if (new_mutex_owner != SimMutex::NO_OWNER)
   {
      // wake up the new owner
      Reply r;

      r.dummy = SyncClient::MUTEX_LOCK_RESPONSE;
      r.time = time;
      m_network.netSend(new_mutex_owner, MCP_RESPONSE_TYPE, (char*)&r, sizeof(r));
   }
}


void SyncServer::condSignal(core_id_t core_id)
{
   carbon_cond_t *cond;
   m_recv_buffer >> cond;

   SubsecondTime time;
   m_recv_buffer >> time;

//printf("SyncServer::condSignal cond(%d)\n", cond);
   SimCond *psimcond = getCond(cond);

   core_id_t woken = psimcond->signal(core_id, time);

   if (woken != INVALID_CORE_ID)
   {
      // wake up the new owner
      // (note: COND_WAIT_RESPONSE == MUTEX_LOCK_RESPONSE, see header)
      Reply r;
      r.dummy = SyncClient::MUTEX_LOCK_RESPONSE;
      r.time = time;
      m_network.netSend(woken, MCP_RESPONSE_TYPE, (char*)&r, sizeof(r));
   }
   else
   {
      // nothing...
   }

   // Alert the signaler
   UInt32 dummy = SyncClient::COND_SIGNAL_RESPONSE;
   m_network.netSend(core_id, MCP_RESPONSE_TYPE, (char*)&dummy, sizeof(dummy));
}

void SyncServer::condBroadcast(core_id_t core_id)
{
   carbon_cond_t *cond;
   m_recv_buffer >> cond;

   SubsecondTime time;
   m_recv_buffer >> time;

//printf("SyncServer::condBroadcast cond(%d)\n", cond);
   SimCond *psimcond = getCond(cond);

   SimCond::WakeupList woken_list;
   psimcond->broadcast(core_id, time, woken_list);

   for (SimCond::WakeupList::iterator it = woken_list.begin(); it != woken_list.end(); it++)
   {
      assert(*it != INVALID_CORE_ID);

      // wake up the new owner
      // (note: COND_WAIT_RESPONSE == MUTEX_LOCK_RESPONSE, see header)
      Reply r;
      r.dummy = SyncClient::MUTEX_LOCK_RESPONSE;
      r.time = time;
      m_network.netSend(*it, MCP_RESPONSE_TYPE, (char*)&r, sizeof(r));
   }

   // Alert the signaler
   UInt32 dummy = SyncClient::COND_BROADCAST_RESPONSE;
   m_network.netSend(core_id, MCP_RESPONSE_TYPE, (char*)&dummy, sizeof(dummy));
}

void SyncServer::barrierInit(core_id_t core_id)
{
   UInt32 count;
   m_recv_buffer >> count;

   m_barriers.push_back(SimBarrier(count));
   UInt32 barrier = (UInt32)m_barriers.size()-1;

   m_network.netSend(core_id, MCP_RESPONSE_TYPE, (char*)&barrier, sizeof(barrier));
}

void SyncServer::barrierWait(core_id_t core_id)
{
   carbon_barrier_t barrier;
   m_recv_buffer >> barrier;

   SubsecondTime time;
   m_recv_buffer >> time;

   LOG_ASSERT_ERROR(barrier < (core_id_t) m_barriers.size(), "barrier = %i, m_barriers.size()= %u", barrier, m_barriers.size());

   SimBarrier *psimbarrier = &m_barriers[barrier];

   SimBarrier::WakeupList woken_list;
   psimbarrier->wait(core_id, time, woken_list);

   SubsecondTime max_time = psimbarrier->getMaxTime();

   for (SimBarrier::WakeupList::iterator it = woken_list.begin(); it != woken_list.end(); it++)
   {
      assert(*it != INVALID_CORE_ID);
      Reply r;
      r.dummy = SyncClient::BARRIER_WAIT_RESPONSE;
      r.time = max_time;
      m_network.netSend(*it, MCP_RESPONSE_TYPE, (char*)&r, sizeof(r));
   }
}

