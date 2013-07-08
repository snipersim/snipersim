#include "network_model_emesh_hop_by_hop.h"
#include "core.h"
#include "simulator.h"
#include "config.h"
#include "utils.h"
#include "packet_type.h"
#include "queue_model_history_list.h"
#include "memory_manager_base.h"
#include "dvfs_manager.h"
#include "stats.h"
#include "config.hpp"

#include <math.h>
#include <stdlib.h>

const char* output_direction_names[] = {
   "up", "down", "left", "right", "---", "self", "peer", "destination"
};
static_assert(NetworkModelEMeshHopByHop::MAX_OUTPUT_DIRECTIONS == sizeof(output_direction_names) / sizeof(output_direction_names[0]),
              "Not enough values in output_direction_names");

const char* OutputDirectionString(NetworkModelEMeshHopByHop::OutputDirection direction)
{
   LOG_ASSERT_ERROR(direction < NetworkModelEMeshHopByHop::MAX_OUTPUT_DIRECTIONS, "Invalid output direction %d", direction);
   return output_direction_names[direction];
}

NetworkModelEMeshHopByHop::NetworkModelEMeshHopByHop(Network* net, EStaticNetwork net_type):
   NetworkModel(net),
   m_enabled(false),
   m_total_bytes_sent(0),
   m_total_packets_sent(0),
   m_total_bytes_received(0),
   m_total_packets_received(0),
   m_total_contention_delay(SubsecondTime::Zero()),
   m_total_packet_latency(SubsecondTime::Zero()),
   m_fake_node(false),
   m_core_id(getNetwork()->getCore()->getId()),
   // Placeholders.  These values will be overwritten in a derived class.
   m_link_bandwidth(Sim()->getDvfsManager()->getCoreDomain(m_core_id), 0),
   m_hop_latency(Sim()->getDvfsManager()->getCoreDomain(m_core_id), 0)
{
   // Get the Link Bandwidth, Hop Latency and if it has broadcast tree mechanism
   try
   {
      // Link Bandwidth is specified in bits/clock_cycle
      m_link_bandwidth = ComponentBandwidthPerCycle(Sim()->getDvfsManager()->getCoreDomain(m_core_id), Sim()->getCfg()->getInt("network/emesh_hop_by_hop/link_bandwidth"));
      // Hop Latency is specified in cycles
      m_hop_latency = ComponentLatency(Sim()->getDvfsManager()->getCoreDomain(m_core_id), Sim()->getCfg()->getInt("network/emesh_hop_by_hop/hop_latency"));

      UInt32 smt_cores = Sim()->getCfg()->getInt("perf_model/core/logical_cpus");
      m_concentration = Sim()->getCfg()->getInt("network/emesh_hop_by_hop/concentration") * smt_cores;
      m_dimensions = Sim()->getCfg()->getInt("network/emesh_hop_by_hop/dimensions");
      m_wrap_around = Sim()->getCfg()->getBool("network/emesh_hop_by_hop/wrap_around");

      // Queue Model enabled? If no, this degrades into a hop counter model
      m_queue_model_enabled = Sim()->getCfg()->getBool("network/emesh_hop_by_hop/queue_model/enabled");
      m_queue_model_type = Sim()->getCfg()->getString("network/emesh_hop_by_hop/queue_model/type");

      m_broadcast_tree_enabled = Sim()->getCfg()->getBool("network/emesh_hop_by_hop/broadcast_tree/enabled");
   }
   catch(...)
   {
      LOG_PRINT_ERROR("Could not read parameters from the configuration file");
   }

   String name = String("network.")+EStaticNetworkStrings[net_type]+".mesh";
   registerStatsMetric(name, m_core_id, "bytes-out", &m_total_bytes_sent);
   registerStatsMetric(name, m_core_id, "packets-out", &m_total_packets_sent);
   registerStatsMetric(name, m_core_id, "bytes-in", &m_total_bytes_received);
   registerStatsMetric(name, m_core_id, "packets-in", &m_total_packets_received);
   registerStatsMetric(name, m_core_id, "contention-delay", &m_total_contention_delay);
   registerStatsMetric(name, m_core_id, "total-delay", &m_total_packet_latency);

   computeMeshDimensions(m_mesh_width, m_mesh_height);

   if (m_core_id % m_concentration != 0 || m_core_id >= m_concentration * m_mesh_width * m_mesh_height)
   {
      m_fake_node = true;
      return;
   }

   createQueueModels(name);
}

NetworkModelEMeshHopByHop::~NetworkModelEMeshHopByHop()
{
   if (m_fake_node)
      return;

   for (UInt32 i = 0; i < NUM_OUTPUT_DIRECTIONS; i++)
   {
      if (m_queue_models[i])
         delete m_queue_models[i];
   }

   delete m_injection_port_queue_model;
   delete m_ejection_port_queue_model;
}

void
NetworkModelEMeshHopByHop::createQueueModels(String name)
{
   SubsecondTime min_processing_time = m_link_bandwidth.getPeriod();

   // Initialize the queue models for all the '4' output directions
   m_queue_models[DOWN] = QueueModel::create(name+".link-down", m_core_id, m_queue_model_type, min_processing_time);
   m_queue_models[LEFT] = QueueModel::create(name+".link-left", m_core_id, m_queue_model_type, min_processing_time);
   m_queue_models[UP] = QueueModel::create(name+".link-up", m_core_id, m_queue_model_type, min_processing_time);
   m_queue_models[RIGHT] = QueueModel::create(name+".link-right", m_core_id, m_queue_model_type, min_processing_time);

   m_injection_port_queue_model = QueueModel::create(name+".link-in", m_core_id, m_queue_model_type, min_processing_time);
   m_ejection_port_queue_model = QueueModel::create(name+".link-out", m_core_id, m_queue_model_type, min_processing_time);
}

void
NetworkModelEMeshHopByHop::routePacket(const NetPacket &pkt, std::vector<Hop> &nextHops)
{
   ScopedLock sl(m_lock);

   core_id_t requester = INVALID_CORE_ID;

   if (pkt.type == SHARED_MEM_1)
      requester = getNetwork()->getCore()->getMemoryManager()->getShmemRequester(pkt.data);
   else // Other Packet types
      requester = pkt.sender;

   LOG_ASSERT_ERROR((requester >= 0) && (requester < (core_id_t) Config::getSingleton()->getTotalCores()),
         "requester(%i)", requester);

   UInt32 pkt_length = getNetwork()->getModeledLength(pkt);

   LOG_PRINT("pkt length(%u)", pkt_length);

   if (pkt.sender == m_core_id)
   {
      m_total_packets_sent ++;
      m_total_bytes_sent += pkt_length;
   }

   if (pkt.receiver == NetPacket::BROADCAST)
   {
      if (m_fake_node)
      {
         for (core_id_t i = 0; i < (core_id_t) Config::getSingleton()->getTotalCores(); i++)
         {
            addHop(DESTINATION, i, i, pkt.time, pkt_length, nextHops, requester);
         }
      }
      else if (m_broadcast_tree_enabled)
      {
         // Injection Port Modeling
         SubsecondTime injection_port_queue_delay = SubsecondTime::Zero();
         if (pkt.sender == m_core_id)
            injection_port_queue_delay = computeInjectionPortQueueDelay(pkt.receiver, pkt.time, pkt_length);
         SubsecondTime curr_time = pkt.time + injection_port_queue_delay;

         // Broadcast tree is enabled
         // Build the broadcast tree
         SInt32 sx, sy, cx, cy;

         computePosition(pkt.sender, sx, sy);
         computePosition(m_core_id, cx, cy);

         if (cy >= sy)
            addHop(UP, NetPacket::BROADCAST, computeCoreId(cx,cy+1), curr_time, pkt_length, nextHops, requester);
         if (cy <= sy)
            addHop(DOWN, NetPacket::BROADCAST, computeCoreId(cx,cy-1), curr_time, pkt_length, nextHops, requester);
         if (cy == sy)
         {
            if (cx >= sx)
               addHop(RIGHT, NetPacket::BROADCAST, computeCoreId(cx+1,cy), curr_time, pkt_length, nextHops, requester);
            if (cx <= sx)
               addHop(LEFT, NetPacket::BROADCAST, computeCoreId(cx-1,cy), curr_time, pkt_length, nextHops, requester);
            if (cx == sx)
               addHop(SELF, m_core_id, m_core_id, curr_time, pkt_length, nextHops, requester);
         }
      }
      else
      {
         // Broadcast tree is not enabled
         // Here, broadcast messages are sent as a collection of unicast messages
         LOG_ASSERT_ERROR(pkt.sender == m_core_id,
               "BROADCAST message to be sent at (%i), original sender(%i), Tree not enabled",
               m_core_id, pkt.sender);

         for (core_id_t i = 0; i < (core_id_t) Config::getSingleton()->getTotalCores(); i++)
         {
            // Injection Port Modeling
            SubsecondTime injection_port_queue_delay = computeInjectionPortQueueDelay(i, pkt.time, pkt_length);
            SubsecondTime curr_time = pkt.time + injection_port_queue_delay;

            // Unicast message to each core
            OutputDirection direction;
            core_id_t next_dest = getNextDest(i, direction);

            addHop(direction, i, next_dest, curr_time, pkt_length, nextHops, requester);
         }
      }
   }
   else if (m_fake_node)
   {
      addHop(DESTINATION, pkt.receiver, pkt.receiver, pkt.time, pkt_length, nextHops, requester);
   }
   else
   {
      // Injection Port Modeling
      SubsecondTime injection_port_queue_delay = SubsecondTime::Zero();
      if (pkt.sender == m_core_id)
      {
         injection_port_queue_delay = computeInjectionPortQueueDelay(pkt.receiver, pkt.time, pkt_length);
         *(subsecond_time_t*)&pkt.queue_delay += injection_port_queue_delay;
      }
      SubsecondTime curr_time = pkt.time + injection_port_queue_delay;

      // A Unicast packet
      OutputDirection direction;
      core_id_t next_dest = getNextDest(pkt.receiver, direction);

      addHop(direction, pkt.receiver, next_dest, curr_time, pkt_length, nextHops, requester, (subsecond_time_t*)&pkt.queue_delay);
   }
}

void
NetworkModelEMeshHopByHop::processReceivedPacket(NetPacket& pkt)
{
   ScopedLock sl(m_lock);

   UInt32 pkt_length = getNetwork()->getModeledLength(pkt);

   core_id_t requester = INVALID_CORE_ID;

   if (pkt.type == SHARED_MEM_1)
      requester = getNetwork()->getCore()->getMemoryManager()->getShmemRequester(pkt.data);
   else // Other Packet types
      requester = pkt.sender;

   LOG_ASSERT_ERROR((requester >= 0) && (requester < (core_id_t) Config::getSingleton()->getTotalCores()),
         "requester(%i)", requester);

   if ( (!m_enabled) || (requester >= (core_id_t) Config::getSingleton()->getApplicationCores())
                     || (m_core_id >= (core_id_t) Config::getSingleton()->getApplicationCores()))
      return;

   SubsecondTime packet_latency = pkt.time - pkt.start_time;
   SubsecondTime contention_delay = packet_latency - (computeDistance(pkt.sender, m_core_id) * m_hop_latency.getLatency());

   if (pkt.sender != m_core_id && !m_fake_node)
   {
      SubsecondTime processing_time = computeProcessingTime(pkt_length);
      SubsecondTime ejection_port_queue_delay = computeEjectionPortQueueDelay(pkt.time, pkt_length);

      packet_latency += (ejection_port_queue_delay + processing_time);
      contention_delay += ejection_port_queue_delay;
      pkt.time += (ejection_port_queue_delay + processing_time);
      pkt.queue_delay += ejection_port_queue_delay;
   }

   m_total_packets_received ++;
   m_total_bytes_received += pkt_length;
   m_total_packet_latency += packet_latency;
   m_total_contention_delay += contention_delay;
}

void
NetworkModelEMeshHopByHop::addHop(OutputDirection direction,
      core_id_t final_dest, core_id_t next_dest,
      SubsecondTime pkt_time, UInt32 pkt_length,
      std::vector<Hop>& nextHops, core_id_t requester,
      subsecond_time_t *queue_delay_stats)
{
   Hop h;
   h.final_dest = final_dest;
   h.next_dest = next_dest;

   if (direction > NUM_OUTPUT_DIRECTIONS)
      h.time = pkt_time;
   else
      h.time = pkt_time + computeLatency(direction, pkt_time, pkt_length, requester, queue_delay_stats);

   nextHops.push_back(h);
}

SInt32
NetworkModelEMeshHopByHop::computeDistance(core_id_t sender, core_id_t receiver)
{
   SInt32 sx, sy, dx, dy;

   computePosition(sender, sx, sy);
   computePosition(receiver, dx, dy);

   if (m_wrap_around)
      return std::min(abs(sx - dx), m_mesh_width - abs(sx - dx))
           + std::min(abs(sy - dy), m_mesh_height - abs(sy - dy));
   else
      return abs(sx - dx) + abs(sy - dy);
}

void
NetworkModelEMeshHopByHop::computePosition(core_id_t core_id, SInt32 &x, SInt32 &y)
{
   x = (core_id / m_concentration) % m_mesh_width;
   y = (core_id / m_concentration) / m_mesh_width;
}

core_id_t
NetworkModelEMeshHopByHop::computeCoreId(SInt32 x, SInt32 y)
{
   x = (x + m_mesh_width) % m_mesh_width;
   y = (y + m_mesh_height) % m_mesh_height;
   return (y * m_mesh_width + x) * m_concentration;
}

SubsecondTime
NetworkModelEMeshHopByHop::computeLatency(OutputDirection direction, SubsecondTime pkt_time, UInt32 pkt_length, core_id_t requester, subsecond_time_t *queue_delay_stats)
{
   LOG_ASSERT_ERROR(!m_fake_node, "Cannot computeLatency on a fake network node");

   LOG_ASSERT_ERROR((direction >= 0) && (direction < NUM_OUTPUT_DIRECTIONS),
         "Invalid Direction(%u)", direction);

   if ( (!m_enabled) || (requester >= (core_id_t) Config::getSingleton()->getApplicationCores()) )
      return SubsecondTime::Zero();

   SubsecondTime processing_time = computeProcessingTime(pkt_length);

   SubsecondTime queue_delay = SubsecondTime::Zero();
   if (m_queue_model_enabled)
   {
      queue_delay = m_queue_models[direction]->computeQueueDelay(pkt_time, processing_time);
      if (queue_delay_stats)
         *queue_delay_stats += queue_delay;
   }

   LOG_PRINT("Queue Delay(%s), Hop Latency(%s)", itostr(queue_delay).c_str(), itostr(m_hop_latency.getLatency()).c_str());
   SubsecondTime packet_latency = m_hop_latency.getLatency() + queue_delay;

   return packet_latency;
}

SubsecondTime
NetworkModelEMeshHopByHop::computeInjectionPortQueueDelay(core_id_t pkt_receiver, SubsecondTime pkt_time, UInt32 pkt_length)
{
   LOG_ASSERT_ERROR(!m_fake_node, "Cannot computeInjectionPortQueueDelay on a fake network node");

   if (!m_queue_model_enabled)
      return SubsecondTime::Zero();

   if (pkt_receiver == m_core_id)
      return SubsecondTime::Zero();

   SubsecondTime processing_time = computeProcessingTime(pkt_length);
   return m_injection_port_queue_model->computeQueueDelay(pkt_time, processing_time);
}

SubsecondTime
NetworkModelEMeshHopByHop::computeEjectionPortQueueDelay(SubsecondTime pkt_time, UInt32 pkt_length)
{
   LOG_ASSERT_ERROR(!m_fake_node, "Cannot computeEjectionPortQueueDelay on a fake network node");

   if (!m_queue_model_enabled)
      return SubsecondTime::Zero();

   SubsecondTime processing_time = computeProcessingTime(pkt_length);
   return m_ejection_port_queue_model->computeQueueDelay(pkt_time, processing_time);
}

SubsecondTime
NetworkModelEMeshHopByHop::computeProcessingTime(UInt32 pkt_length)
{
   LOG_ASSERT_ERROR(!m_fake_node, "Cannot computeProcessingTime on a fake network node");

   // Send: (pkt_length * 8) bits
   // Bandwidth: (m_link_bandwidth) bits/cycle
   UInt32 num_bits = pkt_length * 8;
   return m_link_bandwidth.getRoundedLatency(num_bits);
}

SInt32
NetworkModelEMeshHopByHop::getNextDest(SInt32 final_dest, OutputDirection& direction)
{
   // Do dimension-order routing
   // Curently, do store-and-forward routing
   // FIXME: Should change this to wormhole routing eventually

   if (final_dest >= (core_id_t)Config::getSingleton()->getApplicationCores())
   {
      // We are a fake core (not an application core): warp straight to the destination
      direction = DESTINATION;
      return final_dest;
   }
   else if (m_core_id / m_concentration == final_dest / m_concentration)
   {
      // Destination is self, a peer on our concentrated node
      direction = DESTINATION;
      return final_dest;
   }
   else if (m_fake_node)
   {
      // We are a concentrated node but not the master: first send to master
      direction = PEER;
      return m_core_id - m_core_id % m_concentration;
   }

   SInt32 sx, sy, dx, dy;

   computePosition(m_core_id, sx, sy);
   computePosition(final_dest, dx, dy);

   if ((sx > dx) ^ (m_wrap_around && abs(sx - dx) > (m_mesh_width+1) / 2))
   {
      direction = LEFT;
      return computeCoreId(sx-1,sy);
   }
   else if (sx != dx)
   {
      direction = RIGHT;
      return computeCoreId(sx+1,sy);
   }
   else if ((sy > dy) ^ (m_wrap_around && abs(sy - dy) > (m_mesh_height+1) / 2))
   {
      direction = DOWN;
      return computeCoreId(sx,sy-1);
   }
   else if (sy != dy)
   {
      direction = UP;
      return computeCoreId(sx,sy+1);
   }
   else
   {
      // A send to itself
      direction = SELF;
      return m_core_id;
   }
}

void
NetworkModelEMeshHopByHop::enable()
{
   m_enabled = true;
}

void
NetworkModelEMeshHopByHop::disable()
{
   m_enabled = false;
}

bool
NetworkModelEMeshHopByHop::isEnabled()
{
   return m_enabled;
}

void
NetworkModelEMeshHopByHop::computeMeshDimensions(SInt32 &mesh_width, SInt32 &mesh_height)
{
   SInt32 core_count = Config::getSingleton()->getApplicationCores();
   UInt32 smt_cores = Sim()->getCfg()->getInt("perf_model/core/logical_cpus");
   SInt32 concentration = Sim()->getCfg()->getInt("network/emesh_hop_by_hop/concentration") * smt_cores;
   SInt32 dimensions = Sim()->getCfg()->getInt("network/emesh_hop_by_hop/dimensions");
   String size = Sim()->getCfg()->getString("network/emesh_hop_by_hop/size");

   if (size == "")
   {
      switch(dimensions)
      {
         case 1: // line / ring
            mesh_width = core_count / concentration;
            mesh_height = 1;
            break;
         case 2: // 2-d mesh / torus
            mesh_width = (SInt32) floor (sqrt(core_count / concentration));
            mesh_height = (SInt32) ceil (1.0 * core_count / concentration / mesh_width);
            break;
         default:
            LOG_PRINT_ERROR("Invalid value %d for dimensions, only 1 (line/ring) and 2 (mesh/torus) are currently supported", dimensions);
      }

      LOG_ASSERT_ERROR(core_count == (concentration * mesh_height * mesh_width), "Cannot build a mesh with %d cores (concentration %d), increase NumApplicationCores to %d for a %d x %d mesh", core_count, concentration, concentration * mesh_width * mesh_height, mesh_width, mesh_height);
   }
   else
   {
      // ":"-separated, "," is for heterogeneity
      int res = sscanf(size.c_str(), "%d:%d", &mesh_width, &mesh_height);
      LOG_ASSERT_ERROR(res == 2, "Invalid mesh size \"%s\", expected \"width:height\"", size.c_str());

      LOG_ASSERT_ERROR(core_count == (concentration * mesh_height * mesh_width), "Invalid mesh size %s for %d cores (concentration %d): %d x %d (x %d) == %d != %d", size.c_str(), core_count, concentration, mesh_width, mesh_height, concentration, concentration * mesh_width * mesh_height, core_count);
   }
}

std::pair<bool,SInt32>
NetworkModelEMeshHopByHop::computeCoreCountConstraints(SInt32 core_count)
{
   SInt32 mesh_width, mesh_height;
   computeMeshDimensions(mesh_width, mesh_height);

   assert(core_count <= mesh_width * mesh_height);
   assert(core_count > (mesh_width - 1) * mesh_height);
   assert(core_count > mesh_width * (mesh_height - 1));

   return std::make_pair(true,mesh_height * mesh_width);
}

std::pair<bool, std::vector<core_id_t> >
NetworkModelEMeshHopByHop::computeMemoryControllerPositions(SInt32 num_memory_controllers, SInt32 core_count)
{
   UInt32 smt_cores = Sim()->getCfg()->getInt("perf_model/core/logical_cpus");
   SInt32 concentration = Sim()->getCfg()->getInt("network/emesh_hop_by_hop/concentration") * smt_cores;
   SInt32 dimensions = Sim()->getCfg()->getInt("network/emesh_hop_by_hop/dimensions");
   SInt32 mesh_width, mesh_height;
   computeMeshDimensions(mesh_width, mesh_height);

   // core_id_list_along_perimeter : list of cores along the perimeter of the chip in clockwise order starting from (0,0)
   std::vector<core_id_t> core_id_list_along_perimeter;

   for (SInt32 i = 0; i < mesh_width; i++)
      core_id_list_along_perimeter.push_back(i);

   if (dimensions > 1 && mesh_height > 1)
   {
      for (SInt32 i = 1; i < (mesh_height-1); i++)
         core_id_list_along_perimeter.push_back((i * mesh_width) + mesh_width-1);

      for (SInt32 i = mesh_width-1; i >= 0; i--)
         core_id_list_along_perimeter.push_back(((mesh_height-1) * mesh_width) + i);

      for (SInt32 i = mesh_height-2; i >= 1; i--)
         core_id_list_along_perimeter.push_back(i * mesh_width);

      assert(core_id_list_along_perimeter.size() == (UInt32) (2 * (mesh_width + mesh_height - 2)));
   }

   LOG_ASSERT_ERROR(core_id_list_along_perimeter.size() >= (UInt32) num_memory_controllers,
         "num cores along perimeter(%u), num memory controllers(%i)",
         core_id_list_along_perimeter.size(), num_memory_controllers);

   SInt32 spacing_between_memory_controllers = core_id_list_along_perimeter.size() / num_memory_controllers;

   // core_id_list_with_memory_controllers : list of cores that have memory controllers attached to them
   std::vector<core_id_t> core_id_list_with_memory_controllers;

   for (SInt32 i = 0; i < num_memory_controllers; i++)
   {
      SInt32 index = (i * spacing_between_memory_controllers + mesh_width/2) % core_id_list_along_perimeter.size();
      core_id_list_with_memory_controllers.push_back(concentration * core_id_list_along_perimeter[index]);
   }

   return (std::make_pair(true, core_id_list_with_memory_controllers));
}
