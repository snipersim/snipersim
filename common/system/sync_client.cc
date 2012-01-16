#include "sync_client.h"
#include "network.h"
#include "core.h"
#include "performance_model.h"
#include "packetize.h"
#include "mcp.h"
#include "subsecond_time.h"

#include <iostream>

SyncClient::SyncClient(Core *core)
      : m_core(core)
      , m_network(core->getNetwork())
{
}

SyncClient::~SyncClient()
{
}

void SyncClient::mutexInit(carbon_mutex_t *mux)
{
   // Reset the buffers for the new transmission
   m_recv_buff.clear();
   m_send_buff.clear();

   int msg_type = MCP_MESSAGE_MUTEX_INIT;

   m_send_buff << msg_type << mux;

   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);
   assert(recv_pkt.length == sizeof(carbon_mutex_t *));

   delete [](Byte*) recv_pkt.data;
}

SubsecondTime SyncClient::mutexLock(carbon_mutex_t *mux, SubsecondTime delay)
{
   return __mutexLock(mux, false, delay).first;
}

std::pair<SubsecondTime, bool> SyncClient::mutexTrylock(carbon_mutex_t *mux)
{
   return __mutexLock(mux, true, SubsecondTime::Zero());
}

std::pair<SubsecondTime, bool> SyncClient::__mutexLock(carbon_mutex_t *mux, bool tryLock, SubsecondTime delay)
{
   // Reset the buffers for the new transmission
   m_recv_buff.clear();
   m_send_buff.clear();

   int msg_type = MCP_MESSAGE_MUTEX_LOCK;

   SubsecondTime start_time = m_core->getPerformanceModel()->getElapsedTime() + delay;

   m_send_buff << msg_type << (int)tryLock << mux << start_time;

   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   // Set the CoreState to 'STALLED'
   m_network->getCore()->setState(Core::STALLED);

   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);
   LOG_ASSERT_ERROR(recv_pkt.length == sizeof(Reply), "Packet length is not what was expected %d != %d", recv_pkt.length, sizeof(Reply));

   // Set the CoreState to 'RUNNING'
   m_network->getCore()->setState(Core::WAKING_UP);

   unsigned int dummy;
   SubsecondTime time;
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);
   m_recv_buff >> dummy;

   m_recv_buff >> time;

   if (time > start_time)
       m_core->getPerformanceModel()->queueDynamicInstruction(new SyncInstruction(time - start_time, SyncInstruction::PTHREAD_MUTEX));

   delete [](Byte*) recv_pkt.data;

   if (dummy == MUTEX_LOCK_RESPONSE)
       return std::pair<SubsecondTime, bool>(time > start_time ? time - start_time : SubsecondTime::Zero(), true);
   else if (dummy == MUTEX_TRYLOCK_RESPONSE)
       return std::pair<SubsecondTime, bool>(time > start_time ? time - start_time : SubsecondTime::Zero(), false);
   else
       assert(false);
}

SubsecondTime SyncClient::mutexUnlock(carbon_mutex_t *mux, SubsecondTime delay)
{
   // Reset the buffers for the new transmission
   m_recv_buff.clear();
   m_send_buff.clear();

   int msg_type = MCP_MESSAGE_MUTEX_UNLOCK;

   SubsecondTime start_time = m_core->getPerformanceModel()->getElapsedTime() + delay;

   m_send_buff << msg_type << mux << start_time;

   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);
   assert(recv_pkt.length == sizeof(unsigned int) + sizeof(SubsecondTime));

   unsigned int dummy;
   SubsecondTime time;
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);
   m_recv_buff >> dummy;
   assert(dummy == MUTEX_UNLOCK_RESPONSE);

   m_recv_buff >> time;

   if (time > start_time)
       m_core->getPerformanceModel()->queueDynamicInstruction(new SyncInstruction(time - start_time, SyncInstruction::PTHREAD_MUTEX));

   delete [](Byte*) recv_pkt.data;

   return time > start_time ? time - start_time : SubsecondTime::Zero();
}

void SyncClient::condInit(carbon_cond_t *cond)
{
   // Reset the buffers for the new transmission
   m_recv_buff.clear();
   m_send_buff.clear();

   int msg_type = MCP_MESSAGE_COND_INIT;

   SubsecondTime start_time = m_core->getPerformanceModel()->getElapsedTime();

   m_send_buff << msg_type << cond << start_time;

   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);
   assert(recv_pkt.length == sizeof(carbon_cond_t *));

   delete [](Byte*) recv_pkt.data;
}

SubsecondTime SyncClient::condWait(carbon_cond_t *cond, carbon_mutex_t *mux)
{
   // Reset the buffers for the new transmission
   m_recv_buff.clear();
   m_send_buff.clear();

   int msg_type = MCP_MESSAGE_COND_WAIT;

   SubsecondTime start_time = m_core->getPerformanceModel()->getElapsedTime();

   m_send_buff << msg_type << cond << mux << start_time;

   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   // Set the CoreState to 'STALLED'
   m_network->getCore()->setState(Core::STALLED);

   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);
   assert(recv_pkt.length == sizeof(unsigned int) + sizeof(SubsecondTime));

   // Set the CoreState to 'RUNNING'
   m_network->getCore()->setState(Core::WAKING_UP);

   unsigned int dummy;
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);
   m_recv_buff >> dummy;
   assert(dummy == COND_WAIT_RESPONSE);

   SubsecondTime time;
   m_recv_buff >> time;

   if (time > start_time)
      m_core->getPerformanceModel()->queueDynamicInstruction(new SyncInstruction(time - start_time, SyncInstruction::PTHREAD_COND));

   delete [](Byte*) recv_pkt.data;

   return time > start_time ? time - start_time : SubsecondTime::Zero();
}

SubsecondTime SyncClient::condSignal(carbon_cond_t *cond)
{
   // Reset the buffers for the new transmission
   m_recv_buff.clear();
   m_send_buff.clear();

   int msg_type = MCP_MESSAGE_COND_SIGNAL;

   SubsecondTime start_time = m_core->getPerformanceModel()->getElapsedTime();

   m_send_buff << msg_type << cond << start_time;

   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);
   assert(recv_pkt.length == sizeof(unsigned int));

   unsigned int dummy;
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);
   m_recv_buff >> dummy;
   assert(dummy == COND_SIGNAL_RESPONSE);

   delete [](Byte*) recv_pkt.data;

   return SubsecondTime::Zero();
}

SubsecondTime SyncClient::condBroadcast(carbon_cond_t *cond)
{
   // Reset the buffers for the new transmission
   m_recv_buff.clear();
   m_send_buff.clear();

   int msg_type = MCP_MESSAGE_COND_BROADCAST;

   SubsecondTime start_time = m_core->getPerformanceModel()->getElapsedTime();

   m_send_buff << msg_type << cond << start_time;

   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);
   assert(recv_pkt.length == sizeof(unsigned int));

   unsigned int dummy;
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);
   m_recv_buff >> dummy;
   assert(dummy == COND_BROADCAST_RESPONSE);

   delete [](Byte*) recv_pkt.data;

   return SubsecondTime::Zero();
}

void SyncClient::barrierInit(carbon_barrier_t *barrier, UInt32 count)
{
   // Reset the buffers for the new transmission
   m_recv_buff.clear();
   m_send_buff.clear();

   int msg_type = MCP_MESSAGE_BARRIER_INIT;

   SubsecondTime start_time = m_core->getPerformanceModel()->getElapsedTime();

   m_send_buff << msg_type << count << start_time;

   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);
   assert(recv_pkt.length == sizeof(carbon_barrier_t));

   *barrier = *((carbon_barrier_t*)recv_pkt.data);

   delete [](Byte*) recv_pkt.data;
}

SubsecondTime SyncClient::barrierWait(carbon_barrier_t *barrier)
{
   // Reset the buffers for the new transmission
   m_recv_buff.clear();
   m_send_buff.clear();

   int msg_type = MCP_MESSAGE_BARRIER_WAIT;

   SubsecondTime start_time = m_core->getPerformanceModel()->getElapsedTime();

   m_send_buff << msg_type << *barrier << start_time;

   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   // Set the CoreState to 'STALLED'
   m_network->getCore()->setState(Core::STALLED);

   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);
   assert(recv_pkt.length == sizeof(unsigned int) + sizeof(SubsecondTime));

   // Set the CoreState to 'RUNNING'
   m_network->getCore()->setState(Core::WAKING_UP);

   unsigned int dummy;
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);
   m_recv_buff >> dummy;
   assert(dummy == BARRIER_WAIT_RESPONSE);

   SubsecondTime time;
   m_recv_buff >> time;

   if (time > start_time)
      m_core->getPerformanceModel()->queueDynamicInstruction(new SyncInstruction(time - start_time, SyncInstruction::PTHREAD_BARRIER));

   delete [](Byte*) recv_pkt.data;

   return time > start_time ? time - start_time : SubsecondTime::Zero();
}

