#ifndef __PERF_COUNTER_MANAGER_H__
#define __PERF_COUNTER_MANAGER_H__

#include "thread_manager.h"

class PerfCounterManager
{
   private:
      ThreadManager *m_thread_manager;
      UInt32 m_counter;
      bool m_has_been_reset;
      bool m_has_been_disabled;

   public:
      PerfCounterManager(ThreadManager *thread_manager);
      ~PerfCounterManager();

      void resetCacheCounters(SInt32 sender);
      void disableCacheCounters(SInt32 sender);
};

#endif /* __PERF_COUNTER_MANAGER_H__ */
