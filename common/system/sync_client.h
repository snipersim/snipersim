#ifndef SYNC_CLIENT_H
#define SYNC_CLIENT_H

#include "sync_api.h"
#include "packetize.h"
#include "subsecond_time.h"

class Core;
class Network;

class SyncClient
{
   public:
      SyncClient(Core*);
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

      /* Unique return codes for each function call
         - Note: It is NOT a mistake that
           > COND_WAIT_RESPONSE == MUTEX_LOCK_RESPONSE

           This is necessary because when several threads wait on a
           condition variable and condBroadcast() is called, they will be
           woken by the mutexUnlock() of the thread that holds the lock.

      */

      static const unsigned int MUTEX_LOCK_RESPONSE   = 0xDEADBEEF;
      static const unsigned int MUTEX_TRYLOCK_RESPONSE = 0x0F0FBEEF;
      static const unsigned int MUTEX_UNLOCK_RESPONSE = 0xBABECAFE;
      static const unsigned int COND_WAIT_RESPONSE    = MUTEX_LOCK_RESPONSE;
      static const unsigned int COND_SIGNAL_RESPONSE  = 0xBEEFCAFE;
      static const unsigned int COND_BROADCAST_RESPONSE = 0xDEADCAFE;
      static const unsigned int BARRIER_WAIT_RESPONSE  = 0xCACACAFE;

   private:
      Core *m_core;
      Network *m_network;
      UnstructuredBuffer m_send_buff;
      UnstructuredBuffer m_recv_buff;

      std::pair<SubsecondTime, bool> __mutexLock(carbon_mutex_t *mux, bool tryLock, SubsecondTime delay = SubsecondTime::Zero());

};

#endif
