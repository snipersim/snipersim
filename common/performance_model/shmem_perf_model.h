#ifndef __SHMEM_PERF_MODEL_H__
#define __SHMEM_PERF_MODEL_H__

#include <cassert>
#include <iostream>

#include "lock.h"
#include "subsecond_time.h"

class ShmemPerfModel
{
   public:
      enum Thread_t
      {
         _USER_THREAD = 0,
         _SIM_THREAD,
         NUM_CORE_THREADS
      };

   private:
      SubsecondTime m_elapsed_time[NUM_CORE_THREADS];
      bool m_enabled;
      RwLock m_shmem_perf_model_lock;

      UInt64 m_num_memory_accesses;
      SubsecondTime m_total_memory_access_latency;

   public:
      ShmemPerfModel();
      ~ShmemPerfModel();

      void setElapsedTime(Thread_t thread_num, SubsecondTime time);

      SubsecondTime getElapsedTime(Thread_t thread_num);
      void incrElapsedTime(SubsecondTime time, Thread_t thread_num);
      void updateElapsedTime(SubsecondTime time, Thread_t thread_num);

      void incrTotalMemoryAccessLatency(SubsecondTime shmem_time);

      void enable() { m_enabled = true; }
      void disable() { m_enabled = false; }
      bool isEnabled() { return m_enabled; }
};

#endif /* __SHMEM_PERF_MODEL_H__ */
