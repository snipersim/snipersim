#include "perf_counter_support.h"
#include "core.h"
#include "core_manager.h"
#include "simulator.h"
#include "packet_type.h"
#include "message_types.h"
#include "network.h"

void CarbonResetCacheCounters()
{
   UInt32 msg = MCP_MESSAGE_RESET_CACHE_COUNTERS;
   // Send a message to the MCP asking it to reset all the cache counters
   Network *net = Sim()->getCoreManager()->getCurrentCore()->getNetwork();
   net->netSend(Sim()->getConfig()->getMCPCoreNum(), MCP_SYSTEM_TYPE,
         (const void*) &msg, sizeof(msg));

   NetPacket recv_pkt;
   recv_pkt = net->netRecv(Sim()->getConfig()->getMCPCoreNum(), MCP_RESPONSE_TYPE);
   
   assert(recv_pkt.length == sizeof(UInt32));
   delete [](Byte*)recv_pkt.data;
}

void CarbonDisableCacheCounters()
{
   UInt32 msg = MCP_MESSAGE_DISABLE_CACHE_COUNTERS;
   // Send a message to the MCP asking it to reset all the cache counters
   Network *net = Sim()->getCoreManager()->getCurrentCore()->getNetwork();
   net->netSend(Sim()->getConfig()->getMCPCoreNum(), MCP_SYSTEM_TYPE,
         (const void*) &msg, sizeof(msg));

   NetPacket recv_pkt;
   recv_pkt = net->netRecv(Sim()->getConfig()->getMCPCoreNum(), MCP_RESPONSE_TYPE);
   
   assert(recv_pkt.length == sizeof(UInt32));
   delete [](Byte*)recv_pkt.data;
}
