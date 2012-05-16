#ifndef __TRACE_MANAGER_H
#define __TRACE_MANAGER_H

#include "fixed_types.h"
#include "barrier.h"
#include "semaphore.h"

#include <vector>

class TraceThread;

class TraceManager
{
   private:
      std::vector<TraceThread *> m_threads;
      Barrier *m_barrier;
      Semaphore m_done;
   public:
      TraceManager();
      ~TraceManager();
      void start();
      void stop();
      void wait();
      void run();
      void signalDone(thread_id_t thread_id) { m_done.signal(); }

      UInt64 getProgressExpect();
      UInt64 getProgressValue();
};

#endif // __TRACE_MANAGER_H
