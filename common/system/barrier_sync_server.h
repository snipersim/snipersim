#ifndef __BARRIER_SYNC_SERVER_H__
#define __BARRIER_SYNC_SERVER_H__

#include <vector>

#include "fixed_types.h"
#include "packetize.h"

// Forward Decls
class ThreadManager;
class Network;

class BarrierSyncServer : public ClockSkewMinimizationServer
{
   private:
      Network &m_network;
      UnstructuredBuffer &m_recv_buff;
      ThreadManager* m_thread_manager;

      SubsecondTime m_barrier_interval;
      SubsecondTime m_next_barrier_time;
      std::vector<SubsecondTime> m_local_clock_list;
      std::vector<bool> m_barrier_acquire_list;
      SubsecondTime m_global_time;
      bool m_fastforward;

      UInt32 m_num_application_cores;

   public:
      BarrierSyncServer(Network &network, UnstructuredBuffer &recv_buff);
      ~BarrierSyncServer();

      void processSyncMsg(core_id_t core_id);
      void signal();
      void setFastForward(bool fastforward, SubsecondTime next_barrier_time = SubsecondTime::MaxTime());
      SubsecondTime getGlobalTime() { return m_global_time; }

      void barrierWait(core_id_t core_id);
      bool isBarrierReached(void);
      void barrierRelease(void);
};

#endif /* __BARRIER_SYNC_SERVER_H__ */
