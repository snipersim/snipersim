#ifndef NETWORK_MODEL_MAGIC_H
#define NETWORK_MODEL_MAGIC_H

#include "network.h"
#include "core.h"
#include "lock.h"
#include "subsecond_time.h"

class NetworkModelMagic : public NetworkModel
{
   private:
      bool _enabled;

      Lock _lock;

      UInt64 _num_packets;
      UInt64 _num_bytes;

      ComponentLatency _latency;

   public:
      NetworkModelMagic(Network *net, EStaticNetwork net_type);
      ~NetworkModelMagic() { }

      void routePacket(const NetPacket &pkt, std::vector<Hop> &nextHops);

      void processReceivedPacket(NetPacket& pkt);

      void enable()
      { _enabled = true; }

      void disable()
      { _enabled = false; }
};

#endif /* NETWORK_MODEL_MAGIC_H */
