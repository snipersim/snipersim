#include "trace_manager.h"
#include "trace_thread.h"
#include "simulator.h"
#include "thread_manager.h"
#include "config.hpp"

TraceManager::TraceManager()
   : m_threads()
   , m_done(0)
{
   for(int i = 0; i < Sim()->getCfg()->getInt("traceinput/threads"); ++i)
   {
      String tracefile = Sim()->getCfg()->getString("traceinput/thread_" + itostr(i));
      if (tracefile == "")
         continue;

      TraceThread *thread = new TraceThread(Sim()->getThreadManager()->createThread(), tracefile);
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
   m_done.wait();

   // Then signal all others to stop and wait for them to end
   stop();
}

void TraceManager::run()
{
   start();
   wait();
}

UInt64 TraceManager::getProgressExpect()
{
   return 1000000;
}

UInt64 TraceManager::getProgressValue()
{
   UInt64 value = 0;
   for(std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
      value = std::max(value, 1000000 * (*it)->getProgressValue() / (*it)->getProgressExpect());
   return value;
}
