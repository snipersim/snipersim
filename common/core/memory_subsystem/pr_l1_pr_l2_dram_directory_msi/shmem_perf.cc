#include "shmem_perf.h"
#include "network.h"
#include "log.h"

const char* shmem_reason_names[] = {
   "noc-base",
   "noc-queue",
   "td-access",
   "inv-imbalance",
   "remote-cache-inv",
   "remote-cache-fwd",
   "remote-cache-wb",
   "pending-hit",
   "nuca-tags",
   "nuca-bus",
   "nuca-queue",
   "nuca-data",
   "dram-cache",
   "dram-cache-tags",
   "dram-cache-queue",
   "dram-cache-bus",
   "dram-cache-data",
   "dram",
   "dram-queue",
   "dram-bus",
   "dram-device",
   "unknown",
};

static_assert(ShmemPerf::NUM_SHMEM_TIMES == sizeof(shmem_reason_names) / sizeof(shmem_reason_names[0]),
              "Not enough values in shmem_reason_names");

const char* ShmemReasonString(ShmemPerf::shmem_times_type_t reason)
{
   LOG_ASSERT_ERROR(reason < ShmemPerf::NUM_SHMEM_TIMES, "Invalid ShmemPerf reason %d", reason);
   return shmem_reason_names[reason];
}


ShmemPerf::ShmemPerf()
   : m_core_id(INVALID_CORE_ID)
   , m_time_begin(SubsecondTime::Zero())
   , m_time_last(SubsecondTime::Zero())
   , m_times(NUM_SHMEM_TIMES)
{
}

void ShmemPerf::disable()
{
   // Make sure no-one increments any of our counters anymore
   m_time_last = SubsecondTime::MaxTime();
}

void ShmemPerf::reset(SubsecondTime time, core_id_t core_id)
{
   m_core_id = core_id;
   m_time_begin = time;
   m_time_last = time;
   for(int i = 0; i < ShmemPerf::NUM_SHMEM_TIMES; ++i)
      m_times[i] = SubsecondTime::Zero();
}

void ShmemPerf::updateTime(SubsecondTime time, shmem_times_type_t reason)
{
   // Allow updateTime to be called on a NULL pointer, and do the check here.
   // This works as long as this function is not virtual.
   if (this)
   {
      LOG_ASSERT_ERROR(reason < NUM_SHMEM_TIMES, "Invalid ShmemPerf reason %d", reason);

      // Ignore duplicate paths or updates using stale pointers
      if (time > m_time_last)
      {
         m_times[reason] += time - m_time_last;
         m_time_last = time;
      }
   }
}

void ShmemPerf::updatePacket(NetPacket& packet)
{
   if (this && packet.time > m_time_last)
   {
      m_times[NOC_QUEUE] += packet.queue_delay;
      m_time_last += packet.queue_delay;
      updateTime(packet.time, NOC_BASE);
   }
}

void ShmemPerf::add(ShmemPerf *perf)
{
   for(int i = 0; i < ShmemPerf::NUM_SHMEM_TIMES; ++i)
   {
      ShmemPerf::shmem_times_type_t reason = ShmemPerf::shmem_times_type_t(i);
      m_times[reason] += perf->getComponent(reason);
   }
}
