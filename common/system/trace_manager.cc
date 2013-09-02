#include "trace_manager.h"
#include "trace_thread.h"
#include "simulator.h"
#include "thread_manager.h"
#include "hooks_manager.h"
#include "config.hpp"
#include "sim_api.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

TraceManager::TraceManager()
   : m_monitor(new Monitor(this))
   , m_threads(0)
   , m_num_threads_started(0)
   , m_num_threads_running(0)
   , m_done(0)
   , m_stop_with_first_app(Sim()->getCfg()->getBool("traceinput/stop_with_first_app"))
   , m_app_restart(Sim()->getCfg()->getBool("traceinput/restart_apps"))
   , m_emulate_syscalls(Sim()->getCfg()->getBool("traceinput/emulate_syscalls"))
   , m_num_apps(Sim()->getCfg()->getInt("traceinput/num_apps"))
   , m_num_apps_nonfinish(m_num_apps)
   , m_app_info(m_num_apps)
{
   m_trace_prefix = Sim()->getCfg()->getString("traceinput/trace_prefix");

   if (m_emulate_syscalls)
   {
      if (m_trace_prefix == "")
      {
         std::cerr << "Error: a trace prefix is required when emulating syscalls." << std::endl;
         exit(1);
      }
   }

   if (m_trace_prefix != "")
   {
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
      newThread(i /*app_id*/, true /*first*/, false /*spawn*/, SubsecondTime::Zero(), INVALID_THREAD_ID);
   }
}

String TraceManager::getFifoName(app_id_t app_id, UInt64 thread_num, bool response, bool create)
{
   String filename = m_trace_prefix + (response ? "_response" : "") + ".app" + itostr(app_id) + ".th" + itostr(thread_num) + ".sift";
   if (create)
      mkfifo(filename.c_str(), 0600);
   return filename;
}

thread_id_t TraceManager::createThread(app_id_t app_id, SubsecondTime time, thread_id_t creator_thread_id)
{
   // External version: acquire lock first
   ScopedLock sl(m_lock);

   return newThread(app_id, false /*first*/, true /*spawn*/, time, creator_thread_id);
}

thread_id_t TraceManager::newThread(app_id_t app_id, bool first, bool spawn, SubsecondTime time, thread_id_t creator_thread_id)
{
   // Internal version: assume we're already holding the lock

   assert(static_cast<decltype(app_id)>(m_num_apps) > app_id);

   String tracefile = "", responsefile = "";
   if (first)
   {
      m_app_info[app_id].num_threads = 1;
      m_app_info[app_id].thread_count = 1;
      Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_START, (UInt64)app_id);

      tracefile = m_tracefiles[app_id];
      if (m_responsefiles.size())
         responsefile = m_responsefiles[app_id];
   }
   else
   {
      m_app_info[app_id].num_threads++;
      int thread_num = m_app_info[app_id].thread_count++;

      tracefile = getFifoName(app_id, thread_num, false /*response*/, true /*create*/);
      if (m_responsefiles.size())
         responsefile = getFifoName(app_id, thread_num, true /*response*/, true /*create*/);
   }

   m_num_threads_running++;
   Thread *thread = Sim()->getThreadManager()->createThread(app_id, creator_thread_id);
   TraceThread *tthread = new TraceThread(thread, time, tracefile, responsefile, app_id, first ? false : true /*cleaup*/);
   m_threads.push_back(tthread);

   if (spawn)
   {
      /* First thread of each app spawns only when initialization is done,
         next threads are created once we're running so spawn them right away. */
      tthread->spawn();
   }

   return thread->getId();
}

void TraceManager::signalStarted()
{
   ++m_num_threads_started;
}

void TraceManager::signalDone(TraceThread *thread, SubsecondTime time, bool aborted)
{
   ScopedLock sl(m_lock);

   // Make sure threads don't call signalDone twice (once through endApplication,
   //   and once the regular way), as this would throw off our counts
   if (thread->m_stopped)
   {
      return;
   }
   thread->m_stopped = true;

   app_id_t app_id = thread->getThread()->getAppId();
   m_app_info[app_id].num_threads--;

   if (!aborted)
   {
      if (m_app_info[app_id].num_threads == 0)
      {
         m_app_info[app_id].num_runs++;
         Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_EXIT, (UInt64)app_id);

         if (m_app_info[app_id].num_runs == 1)
            m_num_apps_nonfinish--;

         if (m_stop_with_first_app)
         {
            // First app has ended: stop
            stop();
         }
         else if (m_num_apps_nonfinish == 0)
         {
            // All apps have completed at least once: stop
            stop();
         }
         else
         {
            // Stop condition not met. Restart app?
            if (m_app_restart)
            {
               newThread(app_id, true /*first*/, true /*spawn*/, time, INVALID_THREAD_ID);
            }
         }
      }
   }

   m_num_threads_running--;
}

void TraceManager::endApplication(TraceThread *thread, SubsecondTime time)
{
   for(std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
   {
      // Abort all threads in this application, except ourselves (we should end normally soon)
      if ((*it)->getThread()->getAppId() == thread->getThread()->getAppId() && *it != thread)
      {
         // Ask thread to stop
         (*it)->stop();
         // Threads are often blocked on a futex in this case, so call signalDone in their place
         signalDone(*it, time, true /* aborted */);
      }
   }
}

TraceManager::~TraceManager()
{
   for(std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
      delete *it;
}

void TraceManager::start()
{
   // Begin of region-of-interest when running Sniper inside Sniper
   SimRoiStart();

   m_monitor->spawn();
   for(std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
      (*it)->spawn();
}

void TraceManager::stop()
{
   // End of region-of-interest when running Sniper inside Sniper
   SimRoiEnd();

   // Signal threads to stop.
   for(std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
      (*it)->stop();
   // Give threads some time to end.
   sleep(1);
   // Some threads may be blocked (SIFT reader, syscall, etc.). Don't wait for them or we'll deadlock.
   m_done.signal();
   // Notify SIFT recorders that simulation is done,
   // and that they should hide their errors when writing to an already-closed SIFT pipe.
   mark_done();
}

void TraceManager::mark_done()
{
   FILE *fp = fopen((m_trace_prefix + ".sift_done").c_str(), "w");
   fclose(fp);
}

void TraceManager::wait()
{
   m_done.wait();
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
         if (tthread->m_stopped)
         {
            // FIXME: should we try doing the memory access through another thread in the same application?
            LOG_PRINT_WARNING_ONCE("accessMemory() called but thread already killed since application ended");
            return;
         }
         tthread->handleAccessMemory(lock_signal, mem_op_type, d_addr, data_buffer, data_size);
         return;
      }
   }
   LOG_PRINT_ERROR("Unable to find core %d", core_id);
}

TraceManager::Monitor::Monitor(TraceManager *manager)
   : m_manager(manager)
{
}

TraceManager::Monitor::~Monitor()
{
   delete m_thread;
}

void TraceManager::Monitor::run()
{
   UInt64 n = 0;
   while(true)
   {
      if (m_manager->m_num_threads_started > 0)
         break;

      if (n == 10)
      {
         fprintf(stderr, "[SNIPER] WARNING: No SIFT connections made yet. Waiting...\n");
      }
      else if (n == 60)
      {
         fprintf(stderr, "[SNIPER] ERROR: Could not establish SIFT connection, aborting! Check benchmark-app*.log for errors.\n");
         exit(1);
      }

      sleep(1);
      ++n;
   }
}

void TraceManager::Monitor::spawn()
{
   m_thread = _Thread::create(this);
   m_thread->run();
}
