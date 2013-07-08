#ifndef __NETWORK_MODEL_EMESH_HOP_BY_HOP_GENERIC_H__
#define __NETWORK_MODEL_EMESH_HOP_BY_HOP_GENERIC_H__

#include "network.h"
#include "network_model.h"
#include "fixed_types.h"
#include "queue_model.h"
#include "lock.h"
#include "subsecond_time.h"

class NetworkModelEMeshHopByHop : public NetworkModel
{
   public:
      typedef enum
      {
         UP = 0,
         DOWN,
         LEFT,
         RIGHT,
         NUM_OUTPUT_DIRECTIONS,
         // Directions below are fake and do not have a corresponding queue
         SELF,
         PEER,
         DESTINATION,
         MAX_OUTPUT_DIRECTIONS
      } OutputDirection;

   private:
      // Fields
      SInt32 m_mesh_width;
      SInt32 m_mesh_height;

      QueueModel* m_queue_models[NUM_OUTPUT_DIRECTIONS];
      QueueModel* m_injection_port_queue_model;
      QueueModel* m_ejection_port_queue_model;

      bool m_enabled;

      // Lock
      Lock m_lock;

      // Counters
      UInt64 m_total_bytes_sent;
      UInt64 m_total_packets_sent;
      UInt64 m_total_bytes_received;
      UInt64 m_total_packets_received;
      SubsecondTime m_total_contention_delay;
      SubsecondTime m_total_packet_latency;

      // Functions
      void computePosition(core_id_t core, SInt32 &x, SInt32 &y);
      core_id_t computeCoreId(SInt32 x, SInt32 y);
      SInt32 computeDistance(core_id_t sender, core_id_t receiver);

      void addHop(OutputDirection direction, core_id_t final_dest, core_id_t next_dest, SubsecondTime pkt_time, UInt32 pkt_length, std::vector<Hop>& nextHops, core_id_t requester, subsecond_time_t *queue_delay_stats = NULL);
      SubsecondTime computeLatency(OutputDirection direction, SubsecondTime pkt_time, UInt32 pkt_length, core_id_t requester, subsecond_time_t *queue_delay_stats);
      SubsecondTime computeProcessingTime(UInt32 pkt_length);
      core_id_t getNextDest(core_id_t final_dest, OutputDirection& direction);

      // Injection & Ejection Port Queue Models
      SubsecondTime computeInjectionPortQueueDelay(core_id_t pkt_receiver, SubsecondTime pkt_time, UInt32 pkt_length);
      SubsecondTime computeEjectionPortQueueDelay(SubsecondTime pkt_time, UInt32 pkt_length);

   protected:
      bool m_fake_node; //< True for nodes that are not the master of their concentrated node, these do not count in the topology
      core_id_t m_core_id;
      SInt32 m_concentration; //< Number of cores per network node
      SInt32 m_dimensions; // 1 for line/ring, 2 for mesh/torus
      bool m_wrap_around; // false for line/mesh, true for ring/torus

      ComponentBandwidthPerCycle m_link_bandwidth;
      ComponentLatency m_hop_latency;
      bool m_broadcast_tree_enabled;

      bool m_queue_model_enabled;
      String m_queue_model_type;

      void createQueueModels(String name);

   public:
      NetworkModelEMeshHopByHop(Network* net, EStaticNetwork net_type);
      ~NetworkModelEMeshHopByHop();

      void routePacket(const NetPacket &pkt, std::vector<Hop> &nextHops);
      void processReceivedPacket(NetPacket &pkt);
      static void computeMeshDimensions(SInt32 &mesh_width, SInt32 &mesh_height);
      static std::pair<bool,std::vector<core_id_t> > computeMemoryControllerPositions(SInt32 num_memory_controllers, SInt32 core_count);
      static std::pair<bool,SInt32> computeCoreCountConstraints(SInt32 core_count);

      void enable();
      void disable();
      bool isEnabled();
};

#endif /* __NETWORK_MODEL_EMESH_HOP_BY_HOP_GENERIC_H__ */
