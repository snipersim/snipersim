#ifndef __BARRIER_SYNC_CLIENT_H__
#define __BARRIER_SYNC_CLIENT_H__

#include <cassert>

#include "clock_skew_minimization_object.h"
#include "fixed_types.h"
#include "packetize.h"
#include "subsecond_time.h"

// Forward Decls
class Core;

class BarrierSyncClient : public ClockSkewMinimizationClient
{
   private:
      Core* m_core;

      ComponentLatency m_barrier_interval;
      SubsecondTime m_next_sync_time;

      UInt32 m_num_outstanding;

   public:
      BarrierSyncClient(Core* core);
      ~BarrierSyncClient();

      void enable() {}
      void disable() {}

      void synchronize(SubsecondTime time, bool ignore_time, bool abort_func(void*) = NULL, void* abort_arg = NULL);
      void netProcessSyncMsg(const NetPacket& packet) { assert(false); }

      static const unsigned int BARRIER_RELEASE = 0xBABECAFE;
};

#endif /* __BARRIER_SYNC_CLIENT_H__ */
