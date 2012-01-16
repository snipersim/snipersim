#include <cassert>

#include "barrier_sync_client.h"
#include "simulator.h"
#include "config.h"
#include "message_types.h"
#include "packet_type.h"
#include "packetize.h"
#include "network.h"
#include "core.h"
#include "performance_model.h"
#include "subsecond_time.h"
#include "dvfs_manager.h"
#include "config.hpp"

BarrierSyncClient::BarrierSyncClient(Core* core):
   m_core(core),
   m_barrier_interval(NULL,0), // Will be overwritten below
   m_num_outstanding(0)
{
   try
   {
      m_barrier_interval = ComponentLatency(Sim()->getDvfsManager()->getCoreDomain(core->getId()), Sim()->getCfg()->getInt("clock_skew_minimization/barrier/quantum"));
   }
   catch(...)
   {
      LOG_PRINT_ERROR("Error Reading 'clock_skew_minimization/barrier/quantum' from the config file");
   }
   m_next_sync_time = m_barrier_interval.getLatency();
}

BarrierSyncClient::~BarrierSyncClient()
{}

void
BarrierSyncClient::synchronize(SubsecondTime time, bool ignore_time, bool abort_func(void*), void* abort_arg)
{
   UnstructuredBuffer m_send_buff;
   UnstructuredBuffer m_recv_buff;

   SubsecondTime curr_elapsed_time = time;
   if (time == SubsecondTime::Zero())
      curr_elapsed_time = m_core->getPerformanceModel()->getElapsedTime();

   if (curr_elapsed_time >= m_next_sync_time || ignore_time)
   {
      // Send 'SIM_BARRIER_WAIT' request
      int msg_type = MCP_MESSAGE_CLOCK_SKEW_MINIMIZATION;

      m_send_buff << msg_type << curr_elapsed_time;
      m_core->getNetwork()->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_SYSTEM_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

      m_num_outstanding++;

      LOG_PRINT("Core(%i), curr_elapsed_time(%s), m_next_sync_time(%s) sent SIM_BARRIER_WAIT", m_core->getId(), itostr(curr_elapsed_time).c_str(), itostr(m_next_sync_time).c_str());

      NetPacket recv_pkt;

      while(m_num_outstanding) {
         // Receive 'BARRIER_RELEASE' response

         while(true) {
            recv_pkt = m_core->getNetwork()->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_SYSTEM_RESPONSE_TYPE, abort_func ? 10000000 : 0);
            if (recv_pkt.length == UINT32_MAX) {
               // No barrier release received yet
               if (abort_func && abort_func(abort_arg)) {
                  // Abort requested
                  // Leave m_num_outstanding incremented, so next time we receive the extraneous message
                  // But for now, just exit.
                  return;
               } else
                  continue;
            } else
               break;
         }
         assert(recv_pkt.length == sizeof(int));

         unsigned int dummy;
         m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);
         m_recv_buff >> dummy;
         assert(dummy == BARRIER_RELEASE);

         LOG_PRINT("Core(%i) received SIM_BARRIER_RELEASE", m_core->getId());

         m_num_outstanding--;
      }

      // Update 'm_next_sync_time'
      SubsecondTime barrier_latency = m_barrier_interval.getLatency();
      m_next_sync_time = ((curr_elapsed_time / barrier_latency) * barrier_latency) + barrier_latency;

      // Delete the data buffer
      delete [] (Byte*) recv_pkt.data;
   }
}
