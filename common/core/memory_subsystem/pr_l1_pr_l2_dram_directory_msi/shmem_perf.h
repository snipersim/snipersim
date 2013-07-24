#ifndef __SHMEM_PERF_H
#define __SHMEM_PERF_H

#include "subsecond_time.h"

#include <vector>

class NetPacket;

class ShmemPerf
{
   public:
      typedef enum {
         NOC_BASE,
         NOC_QUEUE,
         TD_ACCESS,
         INV_IMBALANCE,
         REMOTE_CACHE_INV,
         REMOTE_CACHE_WB,
         NUCA_TAGS,
         NUCA_QUEUE,
         NUCA_BUS,
         NUCA_DATA,
         DRAM_CACHE,
         DRAM_CACHE_TAGS,
         DRAM_CACHE_QUEUE,
         DRAM_CACHE_BUS,
         DRAM_CACHE_DATA,
         DRAM,
         DRAM_QUEUE,
         DRAM_BUS,
         DRAM_DEVICE,
         UNKNOWN,
         NUM_SHMEM_TIMES
      } shmem_times_type_t;

      ShmemPerf();
      void disable();
      void reset(SubsecondTime time);
      void updateTime(SubsecondTime time, shmem_times_type_t reason = UNKNOWN);
      void updatePacket(NetPacket& packet);
      void add(ShmemPerf *perf);

      SubsecondTime getInitialTime() const { return m_time_begin; }
      SubsecondTime &getComponent(shmem_times_type_t reason) { return m_times[reason]; }

   private:
      SubsecondTime m_time_begin;
      SubsecondTime m_time_last;
      std::vector<SubsecondTime> m_times;
};

const char* ShmemReasonString(ShmemPerf::shmem_times_type_t reason);

#endif // __SHMEM_PERF_H
