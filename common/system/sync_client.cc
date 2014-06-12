#include "sync_client.h"
#include "sync_server.h"
#include "simulator.h"
#include "thread.h"
#include "thread_manager.h"
#include "core.h"
#include "performance_model.h"
#include "instruction.h"

#include <iostream>

SyncClient::SyncClient(Thread *thread)
      : m_thread(thread)
      , m_server(Sim()->getSyncServer())
{
}

SyncClient::~SyncClient()
{
}

void SyncClient::mutexInit(carbon_mutex_t *mux)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();

   m_server->mutexInit(thread->getId(), mux);
}

SubsecondTime SyncClient::mutexLock(carbon_mutex_t *mux, SubsecondTime delay)
{
   return __mutexLock(mux, false, delay).first;
}

std::pair<SubsecondTime, bool> SyncClient::mutexTrylock(carbon_mutex_t *mux)
{
   return __mutexLock(mux, true, SubsecondTime::Zero());
}

std::pair<SubsecondTime, bool> SyncClient::__mutexLock(carbon_mutex_t *mux, bool tryLock, SubsecondTime delay)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();
   Core *core = thread->getCore();
   SubsecondTime start_time = core->getPerformanceModel()->getElapsedTime() + delay;

   std::pair<SubsecondTime, bool> result = m_server->mutexLock(thread->getId(), mux, tryLock, start_time);

   if (thread->reschedule(result.first, core))
      core = thread->getCore();

   core->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(result.first, SyncInstruction::PTHREAD_MUTEX));

   return std::pair<SubsecondTime, bool>(result.first > start_time ? result.first - start_time : SubsecondTime::Zero(), result.second);
}

SubsecondTime SyncClient::mutexUnlock(carbon_mutex_t *mux, SubsecondTime delay)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();
   SubsecondTime start_time = thread->getCore()->getPerformanceModel()->getElapsedTime() + delay;

   SubsecondTime time = m_server->mutexUnlock(thread->getId(), mux, start_time);

   if (time > start_time)
       thread->getCore()->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(time, SyncInstruction::PTHREAD_MUTEX));

   return time > start_time ? time - start_time : SubsecondTime::Zero();
}

void SyncClient::condInit(carbon_cond_t *cond)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();

   m_server->condInit(thread->getId(), cond);
}

SubsecondTime SyncClient::condWait(carbon_cond_t *cond, carbon_mutex_t *mux)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();
   Core *core = thread->getCore();
   SubsecondTime start_time = core->getPerformanceModel()->getElapsedTime();

   SubsecondTime time = m_server->condWait(thread->getId(), cond, mux, start_time);

   if (thread->reschedule(time, core))
      core = thread->getCore();

   core->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(time, SyncInstruction::PTHREAD_COND));

   return time > start_time ? time - start_time : SubsecondTime::Zero();
}

SubsecondTime SyncClient::condSignal(carbon_cond_t *cond)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();
   SubsecondTime start_time = thread->getCore()->getPerformanceModel()->getElapsedTime();

   m_server->condSignal(thread->getId(), cond, start_time);

   return SubsecondTime::Zero();
}

SubsecondTime SyncClient::condBroadcast(carbon_cond_t *cond)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();
   SubsecondTime start_time = thread->getCore()->getPerformanceModel()->getElapsedTime();

   m_server->condBroadcast(thread->getId(), cond, start_time);

   return SubsecondTime::Zero();
}

void SyncClient::barrierInit(carbon_barrier_t *barrier, UInt32 count)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();

   m_server->barrierInit(thread->getId(), barrier, count);
}

SubsecondTime SyncClient::barrierWait(carbon_barrier_t *barrier)
{
   Thread *thread = Sim()->getThreadManager()->getCurrentThread();
   Core *core = thread->getCore();
   SubsecondTime start_time = core->getPerformanceModel()->getElapsedTime();

   SubsecondTime time = m_server->barrierWait(thread->getId(), barrier, start_time);

   if (thread->reschedule(time, core))
      core = thread->getCore();

   core->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(time, SyncInstruction::PTHREAD_BARRIER));

   return time > start_time ? time - start_time : SubsecondTime::Zero();
}
