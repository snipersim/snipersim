#include "network_model_analytical_server.h"

#include "transport.h"
#include "network.h"
#include "packetize.h"
#include "config.h"
#include "core.h"

#include "log.h"

NetworkModelAnalyticalServer::NetworkModelAnalyticalServer(Network &network,
      UnstructuredBuffer &recv_buffer)
      : _network(network),
      _recv_buffer(recv_buffer)
{
   int num_cores = Config::getSingleton()->getTotalCores();
   _local_utilizations.resize(num_cores);
   for (int i = 0; i < num_cores; i++)
   {
      _local_utilizations[i] = 0.0;
   }
}

NetworkModelAnalyticalServer::~NetworkModelAnalyticalServer()
{ }

struct UtilizationMessage
{
   double ut;
   void *vpmodel;
};

void NetworkModelAnalyticalServer::update(core_id_t core_id)
{
   // extract update
   assert(_recv_buffer.size() == sizeof(UtilizationMessage));
   assert(0 <= core_id && (unsigned int)core_id < Config::getSingleton()->getTotalCores());

   UtilizationMessage *msg;
   msg = (UtilizationMessage*)_recv_buffer.getBuffer();
   assert(msg->vpmodel != NULL);

   _local_utilizations[core_id] = msg->ut;

   // compute global utilization
   double global_utilization = 0.0;
   for (size_t i = 0; i < _local_utilizations.size(); i++)
   {
      global_utilization += _local_utilizations[i];
   }
   global_utilization /= _local_utilizations.size();
   //  assert(0 <= global_utilization && global_utilization <= 1);
   if (global_utilization < 0 || global_utilization > 1)
   {
      LOG_PRINT_WARNING("Unusual network utilization value: %f", global_utilization);
      global_utilization = 0.99;
   }

   UtilizationMessage response_msg = { global_utilization, msg->vpmodel };

   // send response
   NetPacket response;
   response.sender = Config::getSingleton()->getMCPCoreNum();
   response.receiver = core_id;
   response.length = sizeof(response_msg);
   response.type = MCP_UTILIZATION_UPDATE_TYPE;
   response.data = &response_msg;

   _network.netSend(response);
}
