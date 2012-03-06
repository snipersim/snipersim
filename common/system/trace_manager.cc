#include "trace_manager.h"
#include "trace_thread.h"
#include "simulator.h"
#include "config.hpp"

TraceManager::TraceManager()
   : m_threads()
   , m_done(0)
{
   for(core_id_t core_id = 0; core_id < (core_id_t)Sim()->getConfig()->getApplicationCores(); ++core_id)
   {
      String tracefile = Sim()->getCfg()->getString("traceinput/core_" + itostr(core_id), "");
      if (tracefile == "")
         continue;

      TraceThread *thread = new TraceThread(core_id, tracefile);
      m_threads.push_back(thread);
   }

   // Barrier: wait for all threads + ourselves
   m_barrier = new Barrier(m_threads.size() + 1);
}

TraceManager::~TraceManager()
{
   for(std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
      delete *it;
   delete m_barrier;
}

void TraceManager::start()
{
   for(std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
      (*it)->spawn(m_barrier);
   m_barrier->wait();
}

void TraceManager::stop()
{
   // Signal everyone to stop
   for(std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
      (*it)->stop();

   // Wait for everyone to actually finish
   m_barrier->wait();
}

void TraceManager::wait()
{
   // Wait until one thread says it's done
   while(m_done == 0) {}

   // Then signal all others to stop and wait for them to end
   stop();
}

void TraceManager::run()
{
   start();
   wait();
}
