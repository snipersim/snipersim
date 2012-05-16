#ifndef SYNC_SERVER_H
#define SYNC_SERVER_H

#include "fixed_types.h"
#include "sync_api.h"
#include "transport.h"
#include "network.h"
#include "packetize.h"
#include "stable_iterator.h"

#include <queue>
#include <vector>
#include <limits.h>
#include <string.h>
#include <unordered_map>

class SimMutex
{
   public:
      static const thread_id_t NO_OWNER = UINT_MAX;

      SimMutex();
      ~SimMutex();

      // returns true if the lock is owned by someone that is not this thread
      bool isLocked(thread_id_t thread_id);

      // returns the time when this thread owns the lock
      SubsecondTime lock(thread_id_t thread_id, SubsecondTime time);

      // try to take the lock in name of another thread, either waking them or adding them to the list
      bool lock_async(thread_id_t thread_id, thread_id_t thread_by, SubsecondTime time);

      // signals a waiter to continue and returns the next owner
      thread_id_t unlock(thread_id_t thread_id, SubsecondTime time);

   private:
      typedef std::queue<thread_id_t> ThreadQueue;

      ThreadQueue m_waiting;
      thread_id_t m_owner;
};

class SimCond
{

   public:
      SimCond();
      ~SimCond();

      SubsecondTime wait(thread_id_t thread_id, SubsecondTime time, SimMutex * mux);
      thread_id_t signal(thread_id_t thread_id, SubsecondTime time);
      void broadcast(thread_id_t thread_id, SubsecondTime time);

   private:
      class CondWaiter
      {
         public:
            CondWaiter(thread_id_t thread_id, SimMutex * mutex)
                  : m_thread_id(thread_id), m_mutex(mutex) {}
            thread_id_t m_thread_id;
            SimMutex * m_mutex;
      };

      typedef std::queue< CondWaiter > ThreadQueue;
      ThreadQueue m_waiting;
};

class SimBarrier
{
   public:
      SimBarrier(UInt32 count);
      ~SimBarrier();

      SubsecondTime wait(thread_id_t thread_id, SubsecondTime time);

   private:
      typedef std::queue<thread_id_t> ThreadQueue;
      ThreadQueue m_waiting;

      UInt32 m_count;
};

class SyncServer
{
      typedef std::unordered_map<carbon_mutex_t *, SimMutex> MutexVector;
      typedef std::unordered_map<carbon_cond_t *, SimCond> CondVector;
      typedef std::vector<SimBarrier> BarrierVector;

      MutexVector m_mutexes;
      CondVector m_conds;
      BarrierVector m_barriers;

   public:
      SyncServer();
      ~SyncServer();

      void mutexInit(thread_id_t thread_id, carbon_mutex_t *mux);
      std::pair<SubsecondTime, bool> mutexLock(thread_id_t thread_id, carbon_mutex_t *mux, bool tryLock, SubsecondTime time);
      SubsecondTime mutexUnlock(thread_id_t thread_id, carbon_mutex_t *mux, SubsecondTime time);

      void condInit(thread_id_t thread_id, carbon_cond_t *cond);
      SubsecondTime condWait(thread_id_t thread_id, carbon_cond_t *cond, carbon_mutex_t *mux, SubsecondTime time);
      SubsecondTime condSignal(thread_id_t thread_id, carbon_cond_t *cond, SubsecondTime time);
      SubsecondTime condBroadcast(thread_id_t thread_id, carbon_cond_t *cond, SubsecondTime time);

      void barrierInit(thread_id_t thread_id, carbon_barrier_t *barrier, UInt32 count);
      SubsecondTime barrierWait(thread_id_t thread_id, carbon_barrier_t *barrier, SubsecondTime time);

   private:
      SubsecondTime m_reschedule_cost;

      SimMutex * getMutex(carbon_mutex_t * mux, bool canCreate = true);
      SimCond * getCond(carbon_cond_t * cond, bool canCreate = true);
};

#endif // SYNC_SERVER_H
