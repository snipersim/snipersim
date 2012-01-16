#ifndef __RING_SYNC_CLIENT_H__
#define __RING_SYNC_CLIENT_H__

#include "clock_skew_minimization_object.h"
#include "core.h"
#include "lock.h"
#include "cond.h"
#include "fixed_types.h"
#include "subsecond_time.h"

#include <cassert>

class RingSyncClient : public ClockSkewMinimizationClient
{
   private:
      Core* _core;

      SubsecondTime _elapsed_time;
      SubsecondTime _max_elapsed_time;

      Lock _lock;
      ConditionVariable _cond;

   public:
      RingSyncClient(Core* core);
      ~RingSyncClient();

      void enable() {}
      void disable() {}

      void synchronize(SubsecondTime time, bool ignore_time, bool abort_func(void*), void* abort_arg);
      void netProcessSyncMsg(const NetPacket& packet) { assert(false); }

      Lock* getLock() { return &_lock; }
      SubsecondTime getElapsedTime() { return _elapsed_time; }
      void setMaxElapsedTime(SubsecondTime max_elapsed_time);
};

#endif /* __RING_SYNC_CLIENT_H__ */
