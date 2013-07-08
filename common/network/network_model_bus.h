#ifndef NETWORK_MODEL_BUS_H
#define NETWORK_MODEL_BUS_H

#include "network.h"
#include "core.h"
#include "lock.h"
#include "subsecond_time.h"
#include "queue_model.h"
#include "contention_model.h"

class NetworkModelBusGlobal
{
   public:
      Lock _lock;
      const ComponentBandwidth _bandwidth; //< Bits per cycle
      QueueModel* _queue_model;

      UInt64 _num_packets;
      UInt64 _num_packets_delayed;
      UInt64 _num_bytes;
      SubsecondTime _time_used;
      SubsecondTime _total_delay;

      NetworkModelBusGlobal(String name);
      ~NetworkModelBusGlobal();
      SubsecondTime useBus(SubsecondTime t_start, UInt32 length, subsecond_time_t *queue_delay_stats = NULL);
};

class NetworkModelBus : public NetworkModel
{
   static NetworkModelBusGlobal* _bus_global[NUM_STATIC_NETWORKS];

   private:
      bool _enabled;
      NetworkModelBusGlobal* _bus;
      bool _ignore_local;

      bool accountPacket(const NetPacket &pkt);
   public:
      NetworkModelBus(Network *net, EStaticNetwork net_type);
      ~NetworkModelBus() {}

      void routePacket(const NetPacket &pkt, std::vector<Hop> &nextHops);

      void processReceivedPacket(NetPacket& pkt);

      void enable()
      { _enabled = true; }

      void disable()
      { _enabled = false; }
};

#endif /* NETWORK_MODEL_BUS_H */
