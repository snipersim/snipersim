#ifndef __BARRIER_SYNC_SERVER_H__
#define __BARRIER_SYNC_SERVER_H__

#include "fixed_types.h"

#include <unordered_map>

class ThreadManager;

class BarrierSyncServer : public ClockSkewMinimizationServer
{
   private:
      ThreadManager* m_thread_manager;

      SubsecondTime m_barrier_interval;
      SubsecondTime m_next_barrier_time;
      std::unordered_map<thread_id_t, SubsecondTime> m_local_clock_list;
      std::unordered_map<thread_id_t, bool> m_barrier_acquire_list;
      SubsecondTime m_global_time;
      bool m_fastforward;
      volatile bool m_disable;

      bool isBarrierReached(void);
      bool barrierRelease(thread_id_t thread_id = INVALID_THREAD_ID, bool continue_until_release = false);
      void abortBarrier(void);

   public:
      BarrierSyncServer();
      ~BarrierSyncServer();

      virtual void setDisable(bool disable);
      void synchronize(thread_id_t thread_id, SubsecondTime time);
      void signal();
      void release() { abortBarrier(); }
      void advance();
      void setFastForward(bool fastforward, SubsecondTime next_barrier_time = SubsecondTime::MaxTime());
      SubsecondTime getGlobalTime() { return m_global_time; }

      void printState(void);
};

#endif /* __BARRIER_SYNC_SERVER_H__ */
