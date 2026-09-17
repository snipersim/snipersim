#include "trace_manager.h"
#include "trace_thread.h"
#include "simulator.h"
#include "thread_manager.h"
#include "hooks_manager.h"
#include "config.hpp"
#include "sim_api.h"
#include "stats.h"
#include "mimicos.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

// #define DEBUG

TraceManager::TraceManager()
   : m_monitor(new Monitor(this)), m_threads(0), m_num_threads_started(0), m_num_threads_running(0), m_done(0), m_stop_with_first_app(Sim()->getCfg()->getBool("traceinput/stop_with_first_app")), m_app_restart(Sim()->getCfg()->getBool("traceinput/restart_apps")), m_emulate_syscalls(Sim()->getCfg()->getBool("traceinput/emulate_syscalls")), m_num_apps(Sim()->getCfg()->getInt("traceinput/num_apps")), m_num_apps_nonfinish(m_num_apps), m_app_info(m_num_apps), m_tracefiles(m_num_apps), m_responsefiles(m_num_apps)
{
   setupTraceFiles(0);
}

void TraceManager::setupTraceFiles(int index)
{
#ifdef DEBUG
   std::cout << "TraceManager::setupTraceFiles Function" << std::endl;
#endif
   m_trace_prefix = Sim()->getCfg()->getStringArray("traceinput/trace_prefix", index);
#ifdef DEBUG
   std::cout << "trace_prefix: " << m_trace_prefix << std::endl;
#endif

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
     for (UInt32 i = 0; i < m_num_apps; i++)
     {
       m_tracefiles[i] = getFifoName(i, 0, false /*response*/, false /*create*/);
#ifdef DEBUG
       std::cout << "m_tracefiles[" << i << "]: " << m_tracefiles[i] << std::endl;
#endif

       m_responsefiles[i] = getFifoName(i, 0, true /*response*/, false /*create*/);
     }
   }
   else
   {
     for (UInt32 i = 0; i < m_num_apps; i++)
     {
       m_tracefiles[i] = Sim()->getCfg()->getStringArray("traceinput/thread_" + itostr(i), index);
#ifdef DEBUG
       std::cout << "m_tracefiles[" << i << "]: " << m_tracefiles[i] << std::endl;
#endif
     }
   }
}

void TraceManager::init()
{
#ifdef DEBUG
   std::cout << "TraceManager::init Function" << std::endl;
#endif
   // print threads
#ifdef DEBUG
   std::cout << "Number of apps: " << m_num_apps << std::endl;
#endif
   for (UInt32 app_id = 0; app_id < m_num_apps; app_id++)
   {
#ifdef DEBUG
   std::cout << "[TraceManager] init calls newThread for" << 
   " app_id = " << app_id << 
   " creator_thread_id = " << INVALID_THREAD_ID << std::endl;
#endif
     newThread(app_id /*app_id*/, true /*first*/, false /*init_fifo*/, false /*spawn*/, SubsecondTime::Zero(), INVALID_THREAD_ID);
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

#ifdef DEBUG
   std::cout << "[TraceManager] createThread calls newThread for" << 
   " app_id = " << app_id << 
   " creator_thread_id = " << creator_thread_id << std::endl;
#endif
   return newThread(app_id, false /*first*/, true /*init_fifo*/, true /*spawn*/, time, creator_thread_id);
}

thread_id_t TraceManager::newThread(app_id_t app_id, bool first, bool init_fifo, bool spawn, SubsecondTime time, thread_id_t creator_thread_id)
{
   // Internal version: assume we're already holding the lock

   assert(static_cast<decltype(app_id)>(m_num_apps) > app_id);

   // @hsongara: Instantiate applications
   // For non-virtualized environments:
   // Create (only) one App Instance for the Host, this is the application running natively on the system
   // For virtualized environments:
   // Create one App Instance for the application in the VM, and one for the hypervisor application

   if (Sim()->isVirtualizedSystem() == true) {
      app_id_t vm_id = 0;
      Sim()->getMimicOS()->createApplication(vm_id);
      Sim()->getMimicOS_VM()->createApplication(app_id);
   }
   else {
      std::cout << "Creating new application with app id = " << app_id << std::endl;
      Sim()->getMimicOS()->createApplication(app_id);
   }

   String tracefile = "", responsefile = "";
   int thread_num;
   if (first)
   {
     m_app_info[app_id].num_threads = 1;
     m_app_info[app_id].thread_count = 1;

#ifdef DEBUG
     // Print app_id, num_therads, thread_count using std::cout
     std::cout << "[TraceManager] Creating FIRST thread for application " << app_id << " with thread count: " << m_app_info[app_id].thread_count << std::endl;
#endif
     Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_START, (UInt64)app_id);
     Sim()->getStatsManager()->logEvent(StatsManager::EVENT_APP_START, SubsecondTime::MaxTime(), INVALID_CORE_ID, INVALID_THREAD_ID, (UInt64)app_id, 0, "");
     thread_num = 0;

     if (!init_fifo)
     {
       tracefile = m_tracefiles[app_id];
       if (m_responsefiles.size())
         responsefile = m_responsefiles[app_id];
     }
   }
   else
   {
     m_app_info[app_id].num_threads++;
     thread_num = m_app_info[app_id].thread_count++;
#ifdef DEBUG
     // Print app_id, num_therads, thread_count using std::cout
     std::cout << "[TraceManager] Creating NEW thread for application " << app_id << " with thread count: " << m_app_info[app_id].thread_count << std::endl;
#endif
   }

   if (init_fifo)
   {
     tracefile = getFifoName(app_id, thread_num, false /*response*/, true /*create*/);
#ifdef DEBUG
     std::cout << "tracefile: " << tracefile << std::endl;
#endif
     if (m_responsefiles.size()){
       responsefile = getFifoName(app_id, thread_num, true /*response*/, true /*create*/);
#ifdef DEBUG
       std::cout << "responsefile: " << responsefile << std::endl;
#endif
     }

   }

   m_num_threads_running++;
   Thread *thread = Sim()->getThreadManager()->createThread(app_id, creator_thread_id);
   TraceThread *tthread = new TraceThread(thread, time, tracefile, responsefile, app_id, init_fifo /*cleaup*/);
   m_threads.push_back(tthread);

   /* Every thread's reader goes in the pool, so stop() can reach all of them
      -- see the note there.  Whether the last *application* has exited is
      tracked separately, in m_injected_live_count, because this count
      includes kernel threads and so never falls to zero. */
   m_app_reader_pool.registerReader(AppReaderPool::encodeId(app_id, thread_num),
                                    tthread->getSiftReader());

#ifdef DEBUG
   std::cout << "[TraceManager] Trace Thread (tthread)'s current SiftReader is IDentified by name: " << tthread->getSiftReader()->getFilename() << std::endl;
   std::cout << "[TraceManager] Number of Trace Threads in the system: " << m_threads.size()  << std::endl;
#endif

   /* If userspace MimicOS is enabled, we set the current SIFT reader to the Kernel SIFT reader
      for the first thread of the first application, which is the MimicOS thread.
   */

   bool userspace_mimicos_enabled = Sim()->getCfg()->getBool("general/enable_userspace_mimicos");
   /* Under userspace MimicOS every thread reads its own stream, so every
      thread needs its readers wired -- not just the first.  The kernel runs a
      worker per core and creates those workers with pthread_create, which
      reaches Sniper as an ordinary new guest thread; the simulator spawns
      nothing itself.  Leaving the later threads unwired left
      m_current_sift_reader null, and the second kernel worker segfaulted in
      Sift::Reader::Read the moment it started. */
   if (userspace_mimicos_enabled && !(app_id == 0 && thread_num == 0))
   {
      tthread->setKernelSIFTRreader(tthread->getSiftReader());
      tthread->setCurrentSiftReader(tthread->getSiftReader());
   }

   if (app_id == 0 && thread_num == 0)
   {
      // This is the first thread of the first application, set it as the MimicOS if userspace MimicOS is enabled
      std::cout << "[TraceManager] Setting trace readers for the first application" << std::endl;
      if (userspace_mimicos_enabled) {
         // This is the first thread of the first application, set it as the MimicOS thread
         std::cout << "[TraceManager] Setting kernel trace reader for the first application" << std::endl;
         tthread->setKernelSIFTRreader(tthread->getSiftReader());

         std::cout << "[TraceManager] Setting current trace reader for the first application as SIFT App Reader" << std::endl;
         // We set the current sift reader to the default trace reader which represents the MimicOS trace
         tthread->setCurrentSiftReader(tthread->getSiftReader());


         setKernelTraceReader(tthread->getSiftReader());
      }
      else {
         // Default path: set current sift reader to the main trace reader
         // so that handleAccessMemory/getLength/getPosition work correctly
         tthread->setCurrentSiftReader(tthread->getSiftReader());
      }
   }


   if (spawn)
   {
     /* First thread of each app spawns only when initialization is done,
       next threads are created once we're running so spawn them right away. */
     tthread->spawn();
   }

#ifdef DEBUG
   std::cout << "[TraceManager] Thread with ThreadID = " << thread->getId() << " succesfully spawned" << std::endl;
#endif

   return thread->getId();
}

app_id_t TraceManager::createApplication(SubsecondTime time, thread_id_t creator_thread_id)
{
   ScopedLock sl(m_lock);

#ifdef DEBUG
   std::cout << "TraceManager creates new application: " << m_num_apps << std::endl;
#endif

   app_id_t app_id = m_num_apps;
   m_num_apps++;
   m_num_apps_nonfinish++;

   app_info_t app_info;
   m_app_info.push_back(app_info);


   newThread(app_id, true /*first*/, true /*init_fifo*/, true /*spawn*/, time, creator_thread_id);

   return app_id;
}

app_id_t TraceManager::createTraceBasedApplication(SubsecondTime time, char *trace, thread_id_t creator_thread_id)
{

   ScopedLock sl(m_lock);

   if(creator_thread_id == 0){
      std::cout << "[TraceManager] MimicOS creates trace-based application with trace file: " << trace << std::endl;
   }


   // Create a new Sift reader for the trace file - only MimicOS (app 0) can create trace-based applications
#ifdef DEBUG
   std::cout <<  "[TraceManager] Create a new SIFT trace reader (App) for the trace file" <<
                 " - only MimicOS (app_id 0, thread_id = 0)" <<
                 " can create trace-based applications" << std::endl;
#endif

   const UInt32 reader_thread_num = m_app_info[0].num_threads + 1;
   Sift::Reader *app_trace_reader = new Sift::Reader(trace, "", reader_thread_num);

   // App 0 and Thread 0 are always the MimicOS thread
   TraceThread* mimicos_trace_thread = getTraceThread(0, creator_thread_id);

   setTraceReaderHandlers(app_trace_reader, mimicos_trace_thread);
   mimicos_trace_thread->setAppSiftReader(app_trace_reader);

   /* Register the reader so it can be scheduled onto a core later by id.
      Until now an injected reader was reachable only as the injecting
      TraceThread's app reader, so exactly one could exist and only the
      thread that created it could run it.  The pool makes it addressable:
      SimContextSwitchTo(id) installs it on whichever core asks.

      The reader attaches to app 0 -- this function does not allocate an
      app_id, and returns 0 -- so the id is app 0 with the reader's own
      thread number. */
   /* Give the injected application its own id, and key the pool by
      encodeId(app_id, 0).  A kernel asks for the app id and derives the
      thread id as app_id << 16, so the two have to agree: keying by the
      reader's thread number instead leaves the kernel scheduling an id that
      was never registered. */
   const app_id_t injected_app_id = ++m_injected_app_count;
   const AppReaderPool::app_thread_id_t atid =
      AppReaderPool::encodeId(injected_app_id, 0);
   m_app_reader_pool.registerReader(atid, app_trace_reader);
   ++m_injected_live_count;
   mimicos_trace_thread->setInjectedAppId(injected_app_id);

   std::cout << "[TraceManager] Registered injected reader as app_thread_id "
             << atid << " (pool size " << m_app_reader_pool.size() << ")"
             << std::endl;

   /* Announce the new thread to the core that asked for it.  Registering the
      reader only makes it reachable; a kernel still has to be told it exists
      before it will schedule it, and this is the event it waits for.  Without
      it an injected application sits in the pool and never runs -- which is
      what left quantum_expired naming an idle thread. */
   if (Sim()->getMimicOS()->isUserspaceMimicosEnabled())
   {
      core_id_t creator_core = 0;
      if (mimicos_trace_thread && mimicos_trace_thread->getThread()
          && mimicos_trace_thread->getThread()->getCore())
      {
         creator_core = mimicos_trace_thread->getThread()->getCore()->getId();
      }
      Sim()->getMimicOS()->buildMessageWithArgs((int)creator_core,
                                                "new_thread", atid);
   }

   return 0;
}
void TraceManager::signalStarted()
{
   ++m_num_threads_started;
}

void TraceManager::signalDone(TraceThread *thread, SubsecondTime time, bool aborted)
{
#ifdef DEBUG
   std::cout << "TraceManager::signalDone Function" << std::endl;
#endif
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
       Sim()->getStatsManager()->logEvent(StatsManager::EVENT_APP_EXIT, SubsecondTime::MaxTime(), INVALID_CORE_ID, INVALID_THREAD_ID, (UInt64)app_id, 0, "");

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
            newThread(app_id, true /*first*/, false /*init_fifo*/, true /*spawn*/, time, INVALID_THREAD_ID);
         }
       }
     }
   }

   m_num_threads_running--;
}

void TraceManager::endApplication(TraceThread *thread, SubsecondTime time)
{
   for (std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
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

void TraceManager::cleanup()
{
   for (std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
     delete *it;
   m_threads.clear();

   m_num_threads_running = 0;
   m_app_info.clear();
   m_app_info.resize(m_num_apps);
   m_num_apps_nonfinish = m_num_apps;
}

void TraceManager::cleanupAllThreads()
{
   std::cout << "[TraceManager] Cleaning up all trace threads" << std::endl;
   // Clean up ChampSim instruction caches on all trace threads to avoid memory leaks
   for (std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
     (*it)->cleanupChampSimCache();
}

TraceManager::~TraceManager()
{
   cleanup();
}

void TraceManager::start()
{
   m_monitor->spawn();
   for (std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
     (*it)->spawn();
}

void TraceManager::stop()
{
   /* Enter once.  A second pass tells each reader's front end to stop again,
      writing to response pipes the first pass already closed, which
      dereferences a null response stream. */
   bool expected = false;
   if (!m_stop_entered.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel))
   {
      return;
   }
   m_shutdown_requested.store(true, std::memory_order_release);

   // End of region-of-interest when running Sniper inside Sniper
   SimRoiEnd();

   // Signal threads to stop.
   for (std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
     (*it)->stop();

   /* Tell every registered reader's front end to shut down.  A recorder can
      otherwise sit blocked waiting for a magic-instruction response that will
      never come, and fail on a short read during teardown -- which showed up
      as a crash in StatsManager::logEvent from ThreadManager::onThreadExit,
      about one run in three.

      This is why every thread's reader is registered, not only injected ones:
      going through the pool is what reaches them.  Stopping the threads'
      readers directly instead, outside the pool, fixed the crash but hung one
      run in ten -- a thread still reading its own stream does not take kindly
      to having its front end stopped from under it. */
   m_app_reader_pool.stopAll();

   // Give threads some time to end.
   sleep(1);
   
   // Clean up ChampSim instruction caches on all trace threads to avoid memory leaks
   // (TraceThread destructors are never called because TraceManager is never deleted)
   for (std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
     (*it)->cleanupChampSimCache();
   
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
#ifdef DEBUG
   std::cout << "TraceManager::run Function" << std::endl;
#endif
   start();
#ifdef DEBUG
   std::cout << "TraceManager::wait Function" << std::endl;
#endif
   wait();
#ifdef DEBUG
   std::cout << "TraceManager::wait after Function" << std::endl;
#endif
}

UInt64 TraceManager::getProgressExpect()
{
   return 1000000;
}

UInt64 TraceManager::getProgressValue()
{
   UInt64 value = 0;
   for (std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
   {
     uint64_t expect = (*it)->getProgressExpect();
     if (expect)
     {
       UInt64 this_value = 1000000 * (*it)->getProgressValue() / expect;

       if (m_stop_with_first_app)
         // Stop with first app
         value = std::max(value, this_value);
       else
         // Stop with last app
         value = std::min(value, this_value);
     }
   }
   return value;
}

// This should only be called when already holding the thread lock to prevent migrations while we scan for a core id match
void TraceManager::accessMemory(int core_id, Core::lock_signal_t lock_signal, Core::mem_op_t mem_op_type, IntPtr d_addr, char *data_buffer, UInt32 data_size)
{
   for (std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
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
   // Set thread name for Sniper-in-Sniper simulations
   String threadName("trace-monitor");
   SimSetThreadName(threadName.c_str());

   UInt64 n = 0;
   while (true)
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

void TraceManager::endFrontEnd()
{
   for(std::vector<TraceThread *>::iterator it = m_threads.begin(); it != m_threads.end(); ++it){
      if(!(*it)->m_stopped)
         (*it)->frontEndStop();
   }
}

TraceThread* TraceManager::getTraceThread(app_id_t app_id, thread_id_t th)
{
   for (int i = 0; i < (int)m_threads.size(); ++i)
   {
      TraceThread* thread = m_threads[i];
      if (thread->getThread()->getAppId() == app_id && thread->getThread()->getId() == th)
         return thread;

   }
   return nullptr;
}

void TraceManager::setTraceReaderHandlers(Sift::Reader* reader, TraceThread* mimicos_trace_thread) {
   reader->setHandleInstructionCountFunc(TraceThread::__handleInstructionCountFunc, mimicos_trace_thread);
   reader->setHandleCacheOnlyFunc(TraceThread::__handleCacheOnlyFunc, mimicos_trace_thread);
   if (Sim()->getCfg()->getBool("traceinput/mirror_output"))
      reader->setHandleOutputFunc(TraceThread::__handleOutputFunc, mimicos_trace_thread);
   reader->setHandleSyscallFunc(TraceThread::__handleSyscallFunc, mimicos_trace_thread);
   reader->setHandleNewThreadFunc(TraceThread::__handleNewThreadFunc, mimicos_trace_thread);
   reader->setHandleJoinFunc(TraceThread::__handleJoinFunc, mimicos_trace_thread);
   reader->setHandleMagicFunc(TraceThread::__handleMagicFunc, mimicos_trace_thread);
   reader->setHandleEmuFunc(TraceThread::__handleEmuFunc, mimicos_trace_thread);
   reader->setHandleForkFunc(TraceThread::__handleForkFunc, mimicos_trace_thread);
   if (Sim()->getRoutineTracer())
      reader->setHandleRoutineFunc(TraceThread::__handleRoutineChangeFunc, TraceThread::__handleRoutineAnnounceFunc, this);
}
