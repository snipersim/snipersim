#ifndef __TRACE_MANAGER_H
#define __TRACE_MANAGER_H

#include "fixed_types.h"
#include "semaphore.h"
#include "core.h" // for lock_signal_t and mem_op_t
#include "_thread.h"

#include <vector>

class TraceThread;

class TraceManager
{
   private:
      class Monitor : public Runnable
      {
         private:
            void run();
            _Thread *m_thread;
            TraceManager *m_manager;
         public:
            Monitor(TraceManager *manager);
            ~Monitor();
            void spawn();
      };

      struct app_info_t
      {
         app_info_t()
            : thread_count(1)
            , num_threads(1)
            , num_runs(0)
         {}
         UInt32 thread_count;       //< Index counter for new thread's FIFO name
         UInt32 num_threads;        //< Number of active threads for this app (when zero, app is done)
         UInt32 num_runs;           //< Number of completed runs
      };

      Monitor *m_monitor;
      std::vector<TraceThread *> m_threads;
      UInt32 m_num_threads_started;
      UInt32 m_num_threads_running;
      Semaphore m_done;
      const bool m_stop_with_first_app;
      const bool m_app_restart;
      const bool m_emulate_syscalls;
      UInt32 m_num_apps;
      UInt32 m_num_apps_nonfinish;  //< Number of applications that have yet to complete their first run
      std::vector<app_info_t> m_app_info;
      std::vector<String> m_tracefiles;
      std::vector<String> m_responsefiles;
      String m_trace_prefix;
      Lock m_lock;

      String getFifoName(app_id_t app_id, UInt64 thread_num, bool response, bool create);
      thread_id_t newThread(app_id_t app_id, bool first, bool spawn, SubsecondTime time, thread_id_t creator_thread_id);

      friend class Monitor;

   public:
      TraceManager();
      ~TraceManager();
      void init();
      void start();
      void stop();
      void mark_done();
      void wait();
      void run();
      thread_id_t createThread(app_id_t app_id, SubsecondTime time, thread_id_t creator_thread_id);
      void signalStarted();
      void signalDone(TraceThread *thread, SubsecondTime time, bool aborted);
      void endApplication(TraceThread *thread, SubsecondTime time);
      void accessMemory(int core_id, Core::lock_signal_t lock_signal, Core::mem_op_t mem_op_type, IntPtr d_addr, char* data_buffer, UInt32 data_size);

      UInt64 getProgressExpect();
      UInt64 getProgressValue();
};

#endif // __TRACE_MANAGER_H
