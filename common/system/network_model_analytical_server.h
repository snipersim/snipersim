#ifndef NETWORK_MODEL_ANALYTICAL_SERVER_H
#define NETWORK_MODEL_ANALYTICAL_SERVER_H

#include <vector>
#include "fixed_types.h"

class Network;
class UnstructuredBuffer;

class NetworkModelAnalyticalServer
{
   public:
      NetworkModelAnalyticalServer(Network &network, UnstructuredBuffer &recv_buffer);
      ~NetworkModelAnalyticalServer();

      void update(core_id_t);

   private:
      std::vector<double> _local_utilizations;

      Network & _network;
      UnstructuredBuffer & _recv_buffer;
};
#endif
