#ifndef __RANDOM_PAIRS_SYNC_CLIENT_H__
#define __RANDOM_PAIRS_SYNC_CLIENT_H__

#include <sys/time.h>
#include <list>

#include "clock_skew_minimization_object.h"
#include "core.h"
#include "lock.h"
#include "cond.h"
#include "random.h"
#include "fixed_types.h"
#include "subsecond_time.h"

class RandomPairsSyncClient : public ClockSkewMinimizationClient
{
   public:
      class SyncMsg
      {
      public:
         enum MsgType
         {
            REQ = 0,
            ACK,
            WAIT,
            NUM_MSG_TYPES
         };

         core_id_t sender;
         MsgType type;
         SubsecondTime elapsed_time;

         SyncMsg(core_id_t sender, MsgType type, SubsecondTime elapsed_time)
         {
            this->sender = sender;
            this->type = type;
            this->elapsed_time = elapsed_time;
         }
         ~SyncMsg() {}
      };

   private:
      // Data Fields
      Core* _core;

      SubsecondTime _last_sync_elapsed_time;
      ComponentLatency _quantum;
      ComponentLatency _slack;
      float _sleep_fraction;

      struct timeval _start_wall_clock_time;

      std::list<SyncMsg> _msg_queue;
      Lock _lock;
      ConditionVariable _cond;
      Random _rand_num;

      bool _enabled;

      static SubsecondTime MAX_ELAPSED_TIME;

      // Called by user thread
      SubsecondTime userProcessSyncMsgList(void);
      void sendRandomSyncMsg();
      void gotoSleep(const SubsecondTime sleep_time);
      UInt64 getElapsedWallClockTime(void);

      // Called by network thread
      void processSyncReq(const SyncMsg& sync_msg, bool sleeping);


   public:
      RandomPairsSyncClient(Core* core);
      ~RandomPairsSyncClient();

      void enable();
      void disable();

      // Called by user thread
      void synchronize(SubsecondTime time, bool ignore_time, bool abort_func(void*), void* abort_arg);

      // Called by network thread
      void netProcessSyncMsg(const NetPacket& recv_pkt);



};

#endif /* __RANDOM_PAIRS_SYNC_CLIENT_H__ */
