#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include "simulator.h"
#include "thread_manager.h"
#include "core_manager.h"
#include "core.h"
#include "sync_client.h"
#include "config_file.hpp"
#include "thread_support_private.h"
#include "subsecond_time.h"

void CarbonMutexInit(carbon_mutex_t *mux)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   core->getSyncClient()->mutexInit(mux);
}

SubsecondTime CarbonMutexLock(carbon_mutex_t *mux, SubsecondTime delay)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   return core->getSyncClient()->mutexLock(mux, delay);
}

// Defined with C linkage so we cannot return a pair<latency, success> as SyncClient::mutexTrylock does
// SubsecondTime::MaxTime() = not locked
// else = latency
SubsecondTime CarbonMutexTrylock(carbon_mutex_t *mux)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   std::pair<SubsecondTime, bool> res = core->getSyncClient()->mutexTrylock(mux);
   if (res.second)
      return res.first;
   else
      return SubsecondTime::MaxTime();
}

SubsecondTime CarbonMutexUnlock(carbon_mutex_t *mux, SubsecondTime delay)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   return core->getSyncClient()->mutexUnlock(mux, delay);
}

void CarbonCondInit(carbon_cond_t *cond)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   core->getSyncClient()->condInit(cond);
}

SubsecondTime CarbonCondWait(carbon_cond_t *cond, carbon_mutex_t *mux)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   return core->getSyncClient()->condWait(cond, mux);
}

SubsecondTime CarbonCondSignal(carbon_cond_t *cond)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   return core->getSyncClient()->condSignal(cond);
}

SubsecondTime CarbonCondBroadcast(carbon_cond_t *cond)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   return core->getSyncClient()->condBroadcast(cond);
}

void CarbonBarrierInit(carbon_barrier_t *barrier, UInt32 count)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   core->getSyncClient()->barrierInit(barrier, count);
}

SubsecondTime CarbonBarrierWait(carbon_barrier_t *barrier)
{
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   return core->getSyncClient()->barrierWait(barrier);
}
