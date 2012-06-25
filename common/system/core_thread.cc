#include "core_thread.h"
#include "core_manager.h"
#include "performance_model.h"
#include "log.h"
#include "simulator.h"
#include "core.h"
#include "sim_thread_manager.h"

#include <unistd.h>

CoreThread::CoreThread()
   : m_thread(NULL)
{
}

CoreThread::~CoreThread()
{
   delete m_thread;
}

void CoreThread::run()
{
   core_id_t core_id = Sim()->getCoreManager()->registerSimThread(CoreManager::CORE_THREAD);

   LOG_PRINT("Core thread starting...");

   Network *net = Sim()->getCoreManager()->getCoreFromID(core_id)->getNetwork();
   volatile bool cont = true;

   Sim()->getSimThreadManager()->simThreadStartCallback();

   // Turn off cont when we receive a quit message
   net->registerCallback(CORE_THREAD_TERMINATE_THREADS,
                         terminateFunc,
                         (void *)&cont);

   PerformanceModel *prfmdl = Sim()->getCoreManager()->getCurrentCore()->getPerformanceModel();
   while (cont) {
      prfmdl->iterate();
      usleep(1000); // Reduce system load while there's nothing to do (outside ROI)
   }

   Sim()->getSimThreadManager()->simThreadExitCallback();

   LOG_PRINT("Core thread exiting");
}

void CoreThread::spawn()
{
   m_thread = _Thread::create(this);
   m_thread->run();
}

void CoreThread::terminateFunc(void *vp, NetPacket pkt)
{
   bool *pcont = (bool*) vp;
   *pcont = false;
}
