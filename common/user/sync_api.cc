#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include "simulator.h"
#include "thread.h"
#include "sync_client.h"
#include "config_file.hpp"
#include "subsecond_time.h"
#include "thread_manager.h"

void CarbonMutexInit(carbon_mutex_t *mux)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();
   thread->getSyncClient()->mutexInit(mux);
}

SubsecondTime CarbonMutexLock(carbon_mutex_t *mux, SubsecondTime delay)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();
   return thread->getSyncClient()->mutexLock(mux, delay);
}

// Defined with C linkage so we cannot return a pair<latency, success> as SyncClient::mutexTrylock does
// SubsecondTime::MaxTime() = not locked
// else = latency
SubsecondTime CarbonMutexTrylock(carbon_mutex_t *mux)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();
   std::pair<SubsecondTime, bool> res = thread->getSyncClient()->mutexTrylock(mux);
   if (res.second)
      return res.first;
   else
      return SubsecondTime::MaxTime();
}

SubsecondTime CarbonMutexUnlock(carbon_mutex_t *mux, SubsecondTime delay)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();
   return thread->getSyncClient()->mutexUnlock(mux, delay);
}

void CarbonCondInit(carbon_cond_t *cond)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();
   thread->getSyncClient()->condInit(cond);
}

SubsecondTime CarbonCondWait(carbon_cond_t *cond, carbon_mutex_t *mux)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();
   return thread->getSyncClient()->condWait(cond, mux);
}

SubsecondTime CarbonCondSignal(carbon_cond_t *cond)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();
   return thread->getSyncClient()->condSignal(cond);
}

SubsecondTime CarbonCondBroadcast(carbon_cond_t *cond)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();
   return thread->getSyncClient()->condBroadcast(cond);
}

void CarbonBarrierInit(carbon_barrier_t *barrier, UInt32 count)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();
   thread->getSyncClient()->barrierInit(barrier, count);
}

SubsecondTime CarbonBarrierWait(carbon_barrier_t *barrier)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();
   return thread->getSyncClient()->barrierWait(barrier);
}
