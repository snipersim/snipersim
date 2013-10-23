#ifndef NETWORK_MODEL_H
#define NETWORK_MODEL_H

#include "packet_type.h"
#include "fixed_types.h"
#include "subsecond_time.h"

#include <vector>

class NetPacket;
class Network;

// -- Network Models -- //

// To implement a new network model, you must implement this routing
// object. To route, take a packet and compute the next hop(s) and the
// time stamp for when that packet will be forwarded.
//   This lets one implement "magic" networks, analytical models,
// realistic hop-by-hop modeling, as well as broadcast models, such as
// a bus or ATAC.  Each static network has its own model object. This
// lets the user network be modeled accurately, while the MCP is a
// stupid magic network.
//   A packet will be dropped if no hops are filled in the nextHops
// vector.
class NetworkModel
{
   public:
      NetworkModel(Network *network, EStaticNetwork net_type);
      virtual ~NetworkModel() { }

      void countPacket(const NetPacket &packet);

      struct Hop
      {
         SInt32 final_dest;
         SInt32 next_dest;
         subsecond_time_t time;
      };

      virtual void routePacket(const NetPacket &pkt,
                               std::vector<Hop> &nextHops) = 0;
      virtual void processReceivedPacket(NetPacket &pkt) = 0;

      virtual void enable() = 0;
      virtual void disable() = 0;

      static NetworkModel *createModel(Network *network, UInt32 model_type, EStaticNetwork net_type);
      static UInt32 parseNetworkType(String str);

      static std::pair<bool,SInt32> computeCoreCountConstraints(UInt32 network_type, SInt32 core_count);
      static std::pair<bool, std::vector<core_id_t> > computeMemoryControllerPositions(UInt32 network_type, SInt32 num_memory_controllers, SInt32 total_cores);

   protected:
      Network *getNetwork() { return _network; }

   private:
      Network *_network;

      const bool m_collect_traffic_matrix;
      std::vector<uint64_t> m_matrix_packets;
      std::vector<uint64_t> m_matrix_bytes;
};

#endif // NETWORK_MODEL_H
