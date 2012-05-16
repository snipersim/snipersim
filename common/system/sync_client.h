#ifndef SYNC_CLIENT_H
#define SYNC_CLIENT_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include "sync_api.h"

class Thread;
class SyncServer;

class SyncClient
{
   public:
      SyncClient(Thread*);
      ~SyncClient();

      void mutexInit(carbon_mutex_t *mux);
      SubsecondTime mutexLock(carbon_mutex_t *mux, SubsecondTime delay = SubsecondTime::Zero());
      std::pair<SubsecondTime, bool> mutexTrylock(carbon_mutex_t *mux);
      SubsecondTime mutexUnlock(carbon_mutex_t *mux, SubsecondTime delay = SubsecondTime::Zero());

      void condInit(carbon_cond_t *cond);
      SubsecondTime condWait(carbon_cond_t *cond, carbon_mutex_t *mux);
      SubsecondTime condSignal(carbon_cond_t *cond);
      SubsecondTime condBroadcast(carbon_cond_t *cond);

      void barrierInit(carbon_barrier_t *barrier, UInt32 count);
      SubsecondTime barrierWait(carbon_barrier_t *barrier);

   private:
      Thread *m_thread;
      SyncServer *m_server;

      std::pair<SubsecondTime, bool> __mutexLock(carbon_mutex_t *mux, bool tryLock, SubsecondTime delay = SubsecondTime::Zero());
};

#endif
