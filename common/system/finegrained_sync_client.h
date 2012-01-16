#pragma once

#include <cassert>

#include "clock_skew_minimization_object.h"
#include "fixed_types.h"

// Forward Decls
class Core;

class FinegrainedSyncClient : public ClockSkewMinimizationClient
{
   private:
      class SyncerStruct {
         public:
            SubsecondTime time;
            char padding[64 - sizeof(SubsecondTime)];
            SyncerStruct() : time(SubsecondTime::Zero()) {}
      };

      static SyncerStruct *t_last;

      Core *m_core;
      ComponentLatency m_skew;

   public:
      FinegrainedSyncClient(Core* core);
      ~FinegrainedSyncClient();

      void enable() {}
      void disable() {}

      void synchronize(SubsecondTime time, bool ignore_time, bool abort_func(void*), void* abort_arg);
      void netProcessSyncMsg(const NetPacket& packet) { assert(false); }
};
