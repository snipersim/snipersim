#ifndef __TRACE_MANAGER_H
#define __TRACE_MANAGER_H

#include "fixed_types.h"
#include "barrier.h"

#include <vector>

class TraceThread;

class TraceManager
{
   private:
      std::vector<TraceThread *> m_threads;
      Barrier *m_barrier;
      volatile UInt64 m_done;
   public:
      TraceManager();
      ~TraceManager();
      void start();
      void stop();
      void wait();
      void run();
      void signalDone(core_id_t core_id) { atomic_inc_int64(m_done); }
};

#endif // __TRACE_MANAGER_H
