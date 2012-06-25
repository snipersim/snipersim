#include "trace_manager.h"
#include "trace_thread.h"
#include "simulator.h"
#include "thread_manager.h"
#include "config.hpp"

#define MAX_NUM_THREADS 1024

TraceManager::TraceManager()
   : m_threads(MAX_NUM_THREADS)
   , m_done(0)
   , m_thread_count(0)
   , m_stop_with_first_thread(Sim()->getCfg()->getBool("traceinput/stop_with_first_thread"))
{
   newThread(Sim()->getCfg()->getInt("traceinput/threads"), false);
}

thread_id_t TraceManager::newThread(size_t count, bool spawn)
{
   assert((m_thread_count + count) <= MAX_NUM_THREADS);
   for(size_t i = m_thread_count; i < m_thread_count + count; ++i)
   {
      String tracefile = Sim()->getCfg()->getString("traceinput/thread_" + itostr(i));
      if (tracefile == "")
         continue;

      String responsefile = "";

      if (Sim()->getCfg()->getBool("traceinput/emulate_syscalls"))
      {
         responsefile = Sim()->getCfg()->getString("traceinput/thread_response_" + itostr(i));
      }

      TraceThread *thread = new TraceThread(Sim()->getThreadManager()->createThread(), tracefile, responsefile);

      m_threads[i] = thread;

      if (spawn)
      {
         thread->spawn(NULL);
      }
   }
   m_thread_count += count;
   // For now, return the last thread's id
   return m_threads[m_thread_count-1]->getThread()->getId();
}

TraceManager::~TraceManager()
{
   for(size_t i = 0; i < m_thread_count ; ++i)
     delete m_threads[i];
}

void TraceManager::start()
{
   for(size_t i = 0; i < m_thread_count ; ++i)
      m_threads[i]->spawn(NULL);
}

void TraceManager::stop()
{
   // Signal everyone to stop
   for(size_t i = 0; i < m_thread_count ; ++i)
      m_threads[i]->stop();
}

void TraceManager::wait()
{
   // Wait until one thread says it's done
   m_done.wait();

   // Signal all of the other thread contexts to end. This is the default behavior for multi-program workloads,
   // but multi-threaded workloads should disable this.
   if (m_stop_with_first_thread)
   {
      stop();
   }

   for (size_t i = 1 ; i < m_thread_count ; ++i)
   {
      m_done.wait();
   }
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
   for(size_t i = 0; i < m_thread_count ; ++i)
      value = std::max(value, 1000000 * m_threads[i]->getProgressValue() / m_threads[i]->getProgressExpect());
   return value;
}

// This should only be called when already holding the thread lock to prevent migrations while we scan for a core id match
void TraceManager::accessMemory(int core_id, Core::lock_signal_t lock_signal, Core::mem_op_t mem_op_type, IntPtr d_addr, char* data_buffer, UInt32 data_size)
{
   bool found_core = false;
   for (size_t i = 0 ; i < m_thread_count ; i++)
   {
      TraceThread *tthread = m_threads[i];
      assert(tthread != NULL);
      if (tthread->getThread() && tthread->getThread()->getCore() && core_id == tthread->getThread()->getCore()->getId())
      {
         tthread->handleAccessMemory(lock_signal, mem_op_type, d_addr, data_buffer, data_size);
         found_core = true;
      }
   }
   LOG_ASSERT_ERROR(found_core, "Unable to find core %d", core_id);
}
