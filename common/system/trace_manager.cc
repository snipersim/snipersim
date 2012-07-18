#include "trace_manager.h"
#include "trace_thread.h"
#include "simulator.h"
#include "thread_manager.h"
#include "config.hpp"

#define MAX_NUM_THREADS 1024

TraceManager::TraceManager()
   : m_threads(MAX_NUM_THREADS)
   , m_done(0)
   , m_stop_with_first_thread(Sim()->getCfg()->getBool("traceinput/stop_with_first_thread"))
   , m_emulate_syscalls(Sim()->getCfg()->getBool("traceinput/emulate_syscalls"))
   , m_num_apps(Sim()->getCfg()->getInt("traceinput/num_apps"))
   , m_app_thread_count_to_start(m_num_apps, 0)
   , m_app_thread_count_requested(m_num_apps, 0)
   , m_app_thread_count_used(m_num_apps, 0)
{
   UInt32 total_threads_requested = 0;
   for (UInt32 i = 0 ; i < m_num_apps ; i++ )
   {
      m_app_thread_count_to_start[i] = Sim()->getCfg()->getIntArray("traceinput/threads_to_start", i);
      m_app_thread_count_requested[i] = Sim()->getCfg()->getIntArray("traceinput/num_threads", i);
      total_threads_requested += m_app_thread_count_requested[i];
   }
   for (UInt32 i = 0 ; i < total_threads_requested ; i++ )
   {
      m_tracefiles.push_back(Sim()->getCfg()->getString("traceinput/thread_" + itostr(i)));
      if (m_emulate_syscalls)
      {
         m_responsefiles.push_back(Sim()->getCfg()->getString("traceinput/thread_response_" + itostr(i)));
      }
   }
   for (UInt32 i = 0 ; i < m_num_apps ; i++ )
   {
      newThread(/*count*/m_app_thread_count_to_start[i], /*spawn*/false, i);
   }
}

thread_id_t TraceManager::newThread(size_t count, bool spawn, UInt32 app_id)
{
   assert(app_id < m_num_apps);

   UInt32 app_start_thread = 0, start_thread;
   for (UInt32 i = 0 ; i < app_id ; i++)
   {
      app_start_thread += m_app_thread_count_requested[i];
   }
   start_thread = app_start_thread + m_app_thread_count_used[app_id];

   assert((start_thread + count) <= MAX_NUM_THREADS);
   for(size_t i = start_thread; i < start_thread + count; ++i)
   {
      String responsefile = "";

      if (m_emulate_syscalls)
      {
         responsefile = m_responsefiles[i];
      }

      TraceThread *thread = new TraceThread(Sim()->getThreadManager()->createThread(), m_tracefiles[i], responsefile, app_id);

      m_threads[i] = thread;

      if (spawn)
      {
         thread->spawn(NULL);
      }
   }
   m_app_thread_count_used[app_id] += count;
   // For now, return the last thread's id
   return m_threads[app_start_thread+m_app_thread_count_used[app_id]-1]->getThread()->getId();
}

TraceManager::~TraceManager()
{
   UInt32 start_thread = 0;
   for (UInt32 a = 0 ; a < m_num_apps ; a++)
   {
      for (UInt32 j = start_thread ; j < start_thread + m_app_thread_count_used[a] ; j++)
      {
         assert(m_threads[j]);
         delete m_threads[j];
      }
      start_thread += m_app_thread_count_requested[a];
   }
}

void TraceManager::start()
{
   UInt32 start_thread = 0;
   for (UInt32 a = 0 ; a < m_num_apps ; a++)
   {
      for (UInt32 j = start_thread ; j < start_thread + m_app_thread_count_used[a] ; j++)
      {
         m_threads[j]->spawn(NULL);
      }
      start_thread += m_app_thread_count_requested[a];
   }
}

void TraceManager::stop()
{
   // Signal everyone to stop
   UInt32 start_thread = 0;
   for (UInt32 a = 0 ; a < m_num_apps ; a++)
   {
      for (UInt32 j = start_thread ; j < start_thread + m_app_thread_count_used[a] ; j++)
      {
         m_threads[j]->stop();
      }
      start_thread += m_app_thread_count_requested[a];
   }
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

   UInt32 thread_count = 0;
   for (UInt32 a = 0 ; a < m_num_apps ; a++)
   {
      thread_count += m_app_thread_count_used[a];
   }
   for (size_t i = 1 ; i < thread_count ; ++i)
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
   UInt32 start_thread = 0;
   for (UInt32 a = 0 ; a < m_num_apps ; a++)
   {
      for (UInt32 j = start_thread ; j < start_thread + m_app_thread_count_used[a] ; j++)
      {
         uint64_t expect = m_threads[j]->getProgressExpect();
         if (expect)
            value = std::max(value, 1000000 * m_threads[j]->getProgressValue() / expect);
      }
      start_thread += m_app_thread_count_requested[a];
   }
   return value;
}

// This should only be called when already holding the thread lock to prevent migrations while we scan for a core id match
void TraceManager::accessMemory(int core_id, Core::lock_signal_t lock_signal, Core::mem_op_t mem_op_type, IntPtr d_addr, char* data_buffer, UInt32 data_size)
{
   UInt32 start_thread = 0;
   for (UInt32 a = 0 ; a < m_num_apps ; a++)
   {
      for (UInt32 j = start_thread ; j < start_thread + m_app_thread_count_used[a] ; j++)
      {
         TraceThread *tthread = m_threads[j];
         assert(tthread != NULL);
         if (tthread->getThread() && tthread->getThread()->getCore() && core_id == tthread->getThread()->getCore()->getId())
         {
            tthread->handleAccessMemory(lock_signal, mem_op_type, d_addr, data_buffer, data_size);
            return;
         }
      }
      start_thread += m_app_thread_count_requested[a];
   }
   LOG_PRINT_ERROR("Unable to find core %d", core_id);
}
