#include "sim_thread.h"
#include "core_manager.h"
#include "log.h"
#include "simulator.h"
#include "core.h"
#include "sim_thread_manager.h"

SimThread::SimThread()
   : m_thread(NULL)
{
}

SimThread::~SimThread()
{
   delete m_thread;
}

void SimThread::run()
{
   core_id_t core_id = Sim()->getCoreManager()->registerSimThread(CoreManager::SIM_THREAD);

   LOG_PRINT("Sim thread starting...");

   Network *net = Sim()->getCoreManager()->getCoreFromID(core_id)->getNetwork();
   volatile bool cont = true;

   Sim()->getSimThreadManager()->simThreadStartCallback();

   // Turn off cont when we receive a quit message
   net->registerCallback(SIM_THREAD_TERMINATE_THREADS,
                         terminateFunc,
                         (void *)&cont);

   // Actual work gets done here
   while (cont)
      net->netPullFromTransport();

   Sim()->getSimThreadManager()->simThreadExitCallback();

   LOG_PRINT("Sim thread exiting");
}

void SimThread::spawn()
{
   m_thread = _Thread::create(this);
   m_thread->run();
}

void SimThread::terminateFunc(void *vp, NetPacket pkt)
{
   bool *pcont = (bool*) vp;
   *pcont = false;
}
