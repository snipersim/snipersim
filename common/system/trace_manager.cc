#include "trace_manager.h"
#include "trace_thread.h"
#include "simulator.h"
#include "thread_manager.h"
#include "hooks_manager.h"
#include "config.hpp"

#include <sys/types.h>
#include <sys/stat.h>

TraceManager::TraceManager()
   : m_threads(0)
   , m_done(0)
   , m_stop_with_first_thread(Sim()->getCfg()->getBool("traceinput/stop_with_first_thread"))
   , m_emulate_syscalls(Sim()->getCfg()->getBool("traceinput/emulate_syscalls"))
   , m_num_apps(Sim()->getCfg()->getInt("traceinput/num_apps"))
   , m_app_thread_count(m_num_apps, 1)
   , m_app_num_threads(m_num_apps, 1)
{
   if (m_emulate_syscalls)
   {
      m_trace_prefix = Sim()->getCfg()->getString("traceinput/trace_prefix");
      for (UInt32 i = 0 ; i < m_num_apps ; i++ )
      {
         m_tracefiles.push_back(getFifoName(i, 0, false /*response*/, false /*create*/));
         m_responsefiles.push_back(getFifoName(i, 0, true /*response*/, false /*create*/));
      }
   }
   else
   {
      for (UInt32 i = 0 ; i < m_num_apps ; i++ )
      {
         m_tracefiles.push_back(Sim()->getCfg()->getString("traceinput/thread_" + itostr(i)));
      }
   }
}

void TraceManager::init()
{
   for (UInt32 i = 0 ; i < m_num_apps ; i++ )
   {
      newThread(i /*app_id*/, true /*first*/);
   }
}

String TraceManager::getFifoName(app_id_t app_id, UInt64 thread_num, bool response, bool create)
{
   String filename = m_trace_prefix + (response ? "_response" : "") + ".app" + itostr(app_id) + ".th" + itostr(thread_num) + ".sift";
   if (create)
      mkfifo(filename.c_str(), 0600);
   return filename;
}

thread_id_t TraceManager::newThread(app_id_t app_id, bool first)
{
   assert(static_cast<decltype(app_id)>(m_num_apps) > app_id);
   ScopedLock sl(m_lock);

   String tracefile = "", responsefile = "";
   if (first)
   {
      tracefile = m_tracefiles[app_id];
      if (m_emulate_syscalls)
         responsefile = m_responsefiles[app_id];
      Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_START, (UInt64)app_id);
   }
   else
   {
      m_app_num_threads[app_id]++;
      int thread_num = m_app_thread_count[app_id]++;
      tracefile = getFifoName(app_id, thread_num, false /*response*/, true /*create*/);
      if (m_emulate_syscalls)
         responsefile = getFifoName(app_id, thread_num, true /*response*/, true /*create*/);
   }

   Thread *thread = Sim()->getThreadManager()->createThread(app_id);
   TraceThread *tthread = new TraceThread(thread, tracefile, responsefile, app_id, first ? false : true /*cleaup*/);
   m_threads.push_back(tthread);

   if (!first)
   {
      /* First thread of each app spawns only when initialization is done,
         next threads are created once we're running so spawn them right away. */
      tthread->spawn(NULL);
   }

   return thread->getId();
}

void TraceManager::signalDone(Thread *thread)
{
   ScopedLock sl(m_lock);

   app_id_t app_id = thread->getAppId();
   m_app_num_threads[app_id]--;
   if (m_app_num_threads[app_id] == 0)
   {
      Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_EXIT, (UInt64)app_id);
   }

   m_done.signal();
}

TraceManager::~TraceManager()
{
   for(std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
      delete *it;
}

void TraceManager::start()
{
   for(std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
      (*it)->spawn(NULL);
}

void TraceManager::stop()
{
   for(std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
      (*it)->stop();
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

   for (size_t i = 1 ; i < m_threads.size() ; ++i)
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
   for(std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
   {
      uint64_t expect = (*it)->getProgressExpect();
      if (expect)
         value = std::max(value, 1000000 * (*it)->getProgressValue() / expect);
   }
   return value;
}

// This should only be called when already holding the thread lock to prevent migrations while we scan for a core id match
void TraceManager::accessMemory(int core_id, Core::lock_signal_t lock_signal, Core::mem_op_t mem_op_type, IntPtr d_addr, char* data_buffer, UInt32 data_size)
{
   for(std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
   {
      TraceThread *tthread = *it;
      assert(tthread != NULL);
      if (tthread->getThread() && tthread->getThread()->getCore() && core_id == tthread->getThread()->getCore()->getId())
      {
         tthread->handleAccessMemory(lock_signal, mem_op_type, d_addr, data_buffer, data_size);
         return;
      }
   }
   LOG_PRINT_ERROR("Unable to find core %d", core_id);
}
