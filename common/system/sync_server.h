#ifndef SYNC_SERVER_H
#define SYNC_SERVER_H

#include <queue>
#include <vector>
#include <limits.h>
#include <string.h>
#include <unordered_map>

#include "sync_api.h"
#include "transport.h"
#include "network.h"
#include "packetize.h"
#include "stable_iterator.h"

struct Reply
{
   UInt32 dummy;
   subsecond_time_t time;
} __attribute__((packed));

class SimMutex
{
   public:
      static const core_id_t NO_OWNER = UINT_MAX;

      SimMutex();
      ~SimMutex();

      // returns true if the lock is owned by someone that is not this thread
      bool isLocked(core_id_t core_id);

      // returns true if this thread now owns the lock
      bool lock(core_id_t core_id, SubsecondTime time);

      // returns the next owner of the lock so that it can be signaled by
      // the server
      core_id_t unlock(core_id_t core_id, SubsecondTime time);

   private:
      typedef std::queue<core_id_t> ThreadQueue;

      ThreadQueue m_waiting;
      core_id_t m_owner;
};

class SimCond
{

   public:
      typedef std::vector<core_id_t> WakeupList;

      SimCond();
      ~SimCond();

      // returns the thread that gets woken up when the mux is unlocked
      core_id_t wait(core_id_t core_id, SubsecondTime time, SimMutex * mux);
      core_id_t signal(core_id_t core_id, SubsecondTime time);
      void broadcast(core_id_t core_id, SubsecondTime time, WakeupList &woken);

   private:
      class CondWaiter
      {
         public:
            CondWaiter(core_id_t core_id, SimMutex * mutex, SubsecondTime time)
                  : m_core_id(core_id), m_mutex(mutex), m_arrival_time(time) {}
            core_id_t m_core_id;
            SimMutex * m_mutex;
            SubsecondTime m_arrival_time;
      };

      typedef std::vector< CondWaiter > ThreadQueue;
      ThreadQueue m_waiting;
};

class SimBarrier
{
   public:
      typedef std::vector<core_id_t> WakeupList;

      SimBarrier(UInt32 count);
      ~SimBarrier();

      // returns a list of threads to wake up if all have reached barrier
      void wait(core_id_t core_id, SubsecondTime time, WakeupList &woken);
      SubsecondTime getMaxTime() { return m_max_time; }

   private:
      typedef std::vector< core_id_t > ThreadQueue;
      ThreadQueue m_waiting;

      UInt32 m_count;
      SubsecondTime m_max_time;
};

class SyncServer
{
      typedef std::unordered_map<carbon_mutex_t *, SimMutex> MutexVector;
      typedef std::unordered_map<carbon_cond_t *, SimCond> CondVector;
      typedef std::vector<SimBarrier> BarrierVector;

      MutexVector m_mutexes;
      CondVector m_conds;
      BarrierVector m_barriers;

      // FIXME: This should be better organized -- too much redundant crap

   public:
      SyncServer(Network &network, UnstructuredBuffer &recv_buffer);
      ~SyncServer();

      // Remaining parameters to these functions are stored
      // in the recv buffer and get unpacked
      void mutexInit(core_id_t);
      void mutexLock(core_id_t);
      void mutexUnlock(core_id_t);

      void condInit(core_id_t);
      void condWait(core_id_t);
      void condSignal(core_id_t);
      void condBroadcast(core_id_t);

      void barrierInit(core_id_t);
      void barrierWait(core_id_t);

   private:
      Network &m_network;
      UnstructuredBuffer &m_recv_buffer;

      SubsecondTime m_reschedule_cost;

      SimMutex * getMutex(carbon_mutex_t * mux, bool canCreate = true);
      SimCond * getCond(carbon_cond_t * cond, bool canCreate = true);
};

#endif // SYNC_SERVER_H
