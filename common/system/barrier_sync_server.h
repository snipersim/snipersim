#ifndef __BARRIER_SYNC_SERVER_H__
#define __BARRIER_SYNC_SERVER_H__

#include "fixed_types.h"
#include "cond.h"
#include "hooks_manager.h"

#include <vector>

class CoreManager;

class BarrierSyncServer : public ClockSkewMinimizationServer
{
   private:
      SubsecondTime m_barrier_interval;
      SubsecondTime m_next_barrier_time;
      std::vector<SubsecondTime> m_local_clock_list;
      std::vector<bool> m_barrier_acquire_list;
      std::vector<ConditionVariable*> m_core_cond;
      std::vector<core_id_t> m_to_release;
      std::vector<core_id_t> m_core_group;
      std::vector<thread_id_t> m_core_thread;
      SubsecondTime m_global_time;
      bool m_fastforward;
      volatile bool m_disable;

      bool isBarrierReached(void);
      bool barrierRelease(thread_id_t thread_id = INVALID_THREAD_ID, bool continue_until_release = false);
      void abortBarrier(void);
      bool isCoreRunning(core_id_t core_id, bool siblings = true);
      void releaseThread(thread_id_t thread_id);
      void signal();
      void doRelease(int n);

      static SInt64 hookThreadExit(UInt64 object, UInt64 argument) {
         ((BarrierSyncServer*)object)->threadExit((HooksManager::ThreadTime*)argument); return 0;
      }
      static SInt64 hookThreadStall(UInt64 object, UInt64 argument) {
         ((BarrierSyncServer*)object)->threadStall((HooksManager::ThreadStall*)argument); return 0;
      }
      static SInt64 hookThreadMigrate(UInt64 object, UInt64 argument) {
         ((BarrierSyncServer*)object)->threadMigrate((HooksManager::ThreadMigrate*)argument); return 0;
      }
      void threadExit(HooksManager::ThreadTime *argument);
      void threadStall(HooksManager::ThreadStall *argument);
      void threadMigrate(HooksManager::ThreadMigrate *argument);

   public:
      BarrierSyncServer();
      ~BarrierSyncServer();

      virtual void setDisable(bool disable);
      virtual void setGroup(core_id_t core_id, core_id_t master_core_id);
      void synchronize(core_id_t core_id, SubsecondTime time);
      void release() { abortBarrier(); }
      void advance();
      void setFastForward(bool fastforward, SubsecondTime next_barrier_time = SubsecondTime::MaxTime());
      SubsecondTime getGlobalTime(bool upper_bound = false) { return upper_bound ? m_next_barrier_time : m_global_time; }
      void setBarrierInterval(SubsecondTime barrier_interval) { m_barrier_interval = barrier_interval; }
      SubsecondTime getBarrierInterval() const { return m_barrier_interval; }

      void printState(void);
};

#endif /* __BARRIER_SYNC_SERVER_H__ */
