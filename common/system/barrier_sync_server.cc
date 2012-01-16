#include "barrier_sync_client.h"
#include "barrier_sync_server.h"
#include "simulator.h"
#include "thread_manager.h"
#include "hooks_manager.h"
#include "network.h"
#include "config.h"
#include "log.h"
#include "config.hpp"

BarrierSyncServer::BarrierSyncServer(Network &network, UnstructuredBuffer &recv_buff):
   m_network(network),
   m_recv_buff(recv_buff),
   m_global_time(SubsecondTime::Zero()),
   m_fastforward(false)
{
   m_thread_manager = Sim()->getThreadManager();
   try
   {
      m_barrier_interval = SubsecondTime::NS() * (UInt64) Sim()->getCfg()->getInt("clock_skew_minimization/barrier/quantum");
   }
   catch(...)
   {
      LOG_PRINT_ERROR("Error Reading 'clock_skew_minimization/barrier/quantum' from the config file");
   }

   m_next_barrier_time = m_barrier_interval;
   m_num_application_cores = Config::getSingleton()->getApplicationCores();
   m_local_clock_list.resize(m_num_application_cores);
   m_barrier_acquire_list.resize(m_num_application_cores);
   for (UInt32 i = 0; i < m_num_application_cores; i++)
   {
      m_local_clock_list[i] = SubsecondTime::Zero();
      m_barrier_acquire_list[i] = false;
   }
}

BarrierSyncServer::~BarrierSyncServer()
{}

void
BarrierSyncServer::processSyncMsg(core_id_t core_id)
{
   barrierWait(core_id);
}

void
BarrierSyncServer::signal()
{
   if (isBarrierReached())
     barrierRelease();
}

void
BarrierSyncServer::barrierWait(core_id_t core_id)
{
   SubsecondTime time;
   m_recv_buff >> time;

   LOG_PRINT("Received 'SIM_BARRIER_WAIT' from Core(%i), Time(%s)", core_id, itostr(time).c_str());

   LOG_ASSERT_ERROR(m_thread_manager->isThreadRunning(core_id) || m_thread_manager->isThreadInitializing(core_id), "Thread on core(%i) is not running or initializing at time(%s)", core_id, itostr(time).c_str());

   if (time < m_next_barrier_time && !m_fastforward)
   {
      LOG_PRINT("Sent 'SIM_BARRIER_RELEASE' immediately time(%s), m_next_barrier_time(%s)", itostr(time).c_str(), itostr(m_next_barrier_time).c_str());
      // LOG_PRINT_WARNING("core_id(%i), local_clock(%llu), m_next_barrier_time(%llu), m_barrier_interval(%llu)", core_id, time, m_next_barrier_time, m_barrier_interval);
      unsigned int reply = BarrierSyncClient::BARRIER_RELEASE;
      m_network.netSend(core_id, MCP_SYSTEM_RESPONSE_TYPE, (char*) &reply, sizeof(reply));
      return;
   }

   m_local_clock_list[core_id] = time;
   m_barrier_acquire_list[core_id] = true;

   signal();
}

bool
BarrierSyncServer::isBarrierReached()
{
   bool single_thread_barrier_reached = false;

   // Check if all threads have reached the barrier
   // All least one thread must have (sync_time > m_next_barrier_time)
   for (core_id_t core_id = 0; core_id < (core_id_t) m_num_application_cores; core_id++)
   {
      // In fastforward mode, it's enough that a thread is waiting. In detailed mode, it needs to have advanced up to the predefined barrier time
      if (m_fastforward)
      {
         if (m_barrier_acquire_list[core_id])
         {
            // At least one thread has reached the barrier
            single_thread_barrier_reached = true;
         }
         else if (m_thread_manager->isThreadRunning(core_id))
         {
            // Thread is running but hasn't checked in yet. Wait for it to sync.
            return false;
         }
      }
      else if (m_thread_manager->isThreadRunning(core_id))
      {
         if (m_local_clock_list[core_id] < m_next_barrier_time)
         {
            // Thread Running on this core has not reached the barrier
            // Wait for it to sync
            return false;
         }
         else
         {
            // At least one thread has reached the barrier
            single_thread_barrier_reached = true;
         }
      }
   }

   return single_thread_barrier_reached;
}

void
BarrierSyncServer::barrierRelease()
{
   LOG_PRINT("Sending 'BARRIER_RELEASE'");

   // All threads have reached the barrier
   // Advance m_next_barrier_time
   // Release the Barrier

   if (m_fastforward)
   {
      for (core_id_t core_id = 0; core_id < (core_id_t) m_num_application_cores; core_id++)
      {
         // In barrier mode, skip over (potentially very many) timeslots
         if (m_local_clock_list[core_id] > m_next_barrier_time)
            m_next_barrier_time = m_local_clock_list[core_id];
      }
   }

   // If a thread cannot be resumed, we have to advance the sync
   // time till a thread can be resumed. Then only, will we have
   // forward progress

   bool thread_resumed = false;
   while (!thread_resumed)
   {
      m_global_time = m_next_barrier_time;
      Sim()->getHooksManager()->callHooks(HookType::HOOK_PERIODIC, reinterpret_cast<void*>(static_cast<subsecond_time_t>(m_next_barrier_time).m_time));

      m_next_barrier_time += m_barrier_interval;
      LOG_PRINT("m_next_barrier_time updated to (%s)", itostr(m_next_barrier_time).c_str());

      for (core_id_t core_id = 0; core_id < (core_id_t) m_num_application_cores; core_id++)
      {
         if (m_local_clock_list[core_id] < m_next_barrier_time)
         {
            // Check if this core was running. If yes, send a message to that core
            if (m_barrier_acquire_list[core_id] == true)
            {
               //LOG_ASSERT_ERROR(m_thread_manager->isThreadRunning(core_id) || m_thread_manager->isThreadInitializing(core_id), "(%i) has acquired barrier, local_clock(%s), m_next_barrier_time(%s), but not initializing or running", core_id, itostr(m_local_clock_list[core_id]).c_str(), itostr(m_next_barrier_time).c_str());

               unsigned int reply = BarrierSyncClient::BARRIER_RELEASE;
               m_network.netSend(core_id, MCP_SYSTEM_RESPONSE_TYPE, (char*) &reply, sizeof(reply));

               m_barrier_acquire_list[core_id] = false;

               thread_resumed = true;
            }
         }
      }
   }
}

void
BarrierSyncServer::setFastForward(bool fastforward, SubsecondTime next_barrier_time)
{
   m_fastforward = fastforward;
   if (next_barrier_time != SubsecondTime::MaxTime())
      m_next_barrier_time = next_barrier_time;
}
