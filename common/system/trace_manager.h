#ifndef __TRACE_MANAGER_H
#define __TRACE_MANAGER_H

#include "fixed_types.h"
#include "barrier.h"
#include "semaphore.h"
#include "core.h" // for lock_signal_t and mem_op_t

#include <vector>

class TraceThread;

class TraceManager
{
   private:
      std::vector<TraceThread *> m_threads;
      Semaphore m_done;
      bool m_stop_with_first_thread;
      bool m_emulate_syscalls;
      UInt32 m_num_apps;
      std::vector<UInt32> m_app_thread_count_to_start;
      std::vector<UInt32> m_app_thread_count_requested;
      std::vector<UInt32> m_app_thread_count_used;
      std::vector<String> m_tracefiles;
      std::vector<String> m_responsefiles;
   public:
      TraceManager();
      ~TraceManager();
      void start();
      void stop();
      void wait();
      void run();
      thread_id_t newThread(size_t count, bool spawn, UInt32 app_id);
      void signalDone(thread_id_t thread_id) { m_done.signal(); }
      void accessMemory(int core_id, Core::lock_signal_t lock_signal, Core::mem_op_t mem_op_type, IntPtr d_addr, char* data_buffer, UInt32 data_size);

      UInt64 getProgressExpect();
      UInt64 getProgressValue();
};

#endif // __TRACE_MANAGER_H
