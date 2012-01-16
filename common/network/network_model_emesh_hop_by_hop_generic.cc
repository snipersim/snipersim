#include <math.h>
#include <stdlib.h>

#include "network_model_emesh_hop_by_hop_generic.h"
#include "core.h"
#include "simulator.h"
#include "config.h"
#include "utils.h"
#include "packet_type.h"
#include "queue_model_history_list.h"
#include "memory_manager_base.h"
#include "dvfs_manager.h"

NetworkModelEMeshHopByHopGeneric::NetworkModelEMeshHopByHopGeneric(Network* net):
   NetworkModel(net),
   m_enabled(false),
   m_total_bytes_received(0),
   m_total_packets_received(0),
   m_total_contention_delay(SubsecondTime::Zero()),
   m_total_packet_latency(SubsecondTime::Zero()),
   m_core_id(getNetwork()->getCore()->getId()),
   // Placeholders.  These values will be overwritten in a derived class.
   m_link_bandwidth(Sim()->getDvfsManager()->getCoreDomain(m_core_id), 0),
   m_hop_latency(Sim()->getDvfsManager()->getCoreDomain(m_core_id), 0)
{
   SInt32 total_cores = Config::getSingleton()->getTotalCores();

   m_mesh_width = (SInt32) floor (sqrt(total_cores));
   m_mesh_height = (SInt32) ceil (1.0 * total_cores / m_mesh_width);

   assert(total_cores == (m_mesh_width * m_mesh_height));
}

NetworkModelEMeshHopByHopGeneric::~NetworkModelEMeshHopByHopGeneric()
{
   for (UInt32 i = 0; i < NUM_OUTPUT_DIRECTIONS; i++)
   {
      if (m_queue_models[i])
         delete m_queue_models[i];
   }

   delete m_injection_port_queue_model;
   delete m_ejection_port_queue_model;
}

void
NetworkModelEMeshHopByHopGeneric::createQueueModels()
{
   SubsecondTime min_processing_time = m_link_bandwidth.getPeriod();
   // Initialize the queue models for all the '4' output directions
   for (UInt32 direction = 0; direction < NUM_OUTPUT_DIRECTIONS; direction ++)
   {
      m_queue_models[direction] = NULL;
   }

   if ((m_core_id / m_mesh_width) != 0)
   {
      m_queue_models[DOWN] = QueueModel::create("network-down", m_core_id, m_queue_model_type, min_processing_time);
   }
   if ((m_core_id % m_mesh_width) != 0)
   {
      m_queue_models[LEFT] = QueueModel::create("network-left", m_core_id, m_queue_model_type, min_processing_time);
   }
   if ((m_core_id / m_mesh_width) != (m_mesh_height - 1))
   {
      m_queue_models[UP] = QueueModel::create("network-up", m_core_id, m_queue_model_type, min_processing_time);
   }
   if ((m_core_id % m_mesh_width) != (m_mesh_width - 1))
   {
      m_queue_models[RIGHT] = QueueModel::create("network-right", m_core_id, m_queue_model_type, min_processing_time);
   }

   m_injection_port_queue_model = QueueModel::create("network-in", m_core_id, m_queue_model_type, min_processing_time);
   m_ejection_port_queue_model = QueueModel::create("network-out", m_core_id, m_queue_model_type, min_processing_time);
}

void
NetworkModelEMeshHopByHopGeneric::routePacket(const NetPacket &pkt, std::vector<Hop> &nextHops)
{
   ScopedLock sl(m_lock);

   core_id_t requester = INVALID_CORE_ID;

   if ((pkt.type == SHARED_MEM_1) || (pkt.type == SHARED_MEM_2))
      requester = getNetwork()->getCore()->getMemoryManager()->getShmemRequester(pkt.data);
   else // Other Packet types
      requester = pkt.sender;

   LOG_ASSERT_ERROR((requester >= 0) && (requester < (core_id_t) Config::getSingleton()->getTotalCores()),
         "requester(%i)", requester);

   UInt32 pkt_length = getNetwork()->getModeledLength(pkt);

   LOG_PRINT("pkt length(%u)", pkt_length);

   if (pkt.receiver == NetPacket::BROADCAST)
   {
      if (m_broadcast_tree_enabled)
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
   else
   {
      // Injection Port Modeling
      SubsecondTime injection_port_queue_delay = SubsecondTime::Zero();
      if (pkt.sender == m_core_id)
         injection_port_queue_delay = computeInjectionPortQueueDelay(pkt.receiver, pkt.time, pkt_length);
      SubsecondTime curr_time = pkt.time + injection_port_queue_delay;

      // A Unicast packet
      OutputDirection direction;
      core_id_t next_dest = getNextDest(pkt.receiver, direction);

      addHop(direction, pkt.receiver, next_dest, curr_time, pkt_length, nextHops, requester);
   }
}

void
NetworkModelEMeshHopByHopGeneric::processReceivedPacket(NetPacket& pkt)
{
   ScopedLock sl(m_lock);

   UInt32 pkt_length = getNetwork()->getModeledLength(pkt);

   core_id_t requester = INVALID_CORE_ID;

   if ((pkt.type == SHARED_MEM_1) || (pkt.type == SHARED_MEM_2))
      requester = getNetwork()->getCore()->getMemoryManager()->getShmemRequester(pkt.data);
   else // Other Packet types
      requester = pkt.sender;

   LOG_ASSERT_ERROR((requester >= 0) && (requester < (core_id_t) Config::getSingleton()->getTotalCores()),
         "requester(%i)", requester);

   if ( (!m_enabled) || (requester >= (core_id_t) Config::getSingleton()->getApplicationCores()) )
      return;

   SubsecondTime packet_latency = pkt.time - pkt.start_time;
   SubsecondTime contention_delay = packet_latency - (computeDistance(pkt.sender, m_core_id) * m_hop_latency.getLatency());

   if (pkt.sender != m_core_id)
   {
      SubsecondTime processing_time = computeProcessingTime(pkt_length);
      SubsecondTime ejection_port_queue_delay = computeEjectionPortQueueDelay(pkt.time, pkt_length);

      packet_latency += (ejection_port_queue_delay + processing_time);
      contention_delay += ejection_port_queue_delay;
      pkt.time += (ejection_port_queue_delay + processing_time);
   }

   m_total_packets_received ++;
   m_total_bytes_received += pkt_length;
   m_total_packet_latency += packet_latency;
   m_total_contention_delay += contention_delay;
}

void
NetworkModelEMeshHopByHopGeneric::addHop(OutputDirection direction,
      core_id_t final_dest, core_id_t next_dest,
      SubsecondTime pkt_time, UInt32 pkt_length,
      std::vector<Hop>& nextHops, core_id_t requester)
{
   LOG_ASSERT_ERROR((direction == SELF) || ((direction >= 0) && (direction < NUM_OUTPUT_DIRECTIONS)),
         "Invalid Direction(%u)", direction);

   if ((direction == SELF) || m_queue_models[direction])
   {
      Hop h;
      h.final_dest = final_dest;
      h.next_dest = next_dest;

      if (direction == SELF)
         h.time = pkt_time;
      else
         h.time = pkt_time + computeLatency(direction, pkt_time, pkt_length, requester);

      nextHops.push_back(h);
   }
}

SInt32
NetworkModelEMeshHopByHopGeneric::computeDistance(core_id_t sender, core_id_t receiver)
{
   SInt32 sx, sy, dx, dy;

   computePosition(sender, sx, sy);
   computePosition(receiver, dx, dy);

   return abs(sx - dx) + abs(sy - dy);
}

void
NetworkModelEMeshHopByHopGeneric::computePosition(core_id_t core_id, SInt32 &x, SInt32 &y)
{
   x = core_id % m_mesh_width;
   y = core_id / m_mesh_width;
}

core_id_t
NetworkModelEMeshHopByHopGeneric::computeCoreId(SInt32 x, SInt32 y)
{
   return (y * m_mesh_width + x);
}

SubsecondTime
NetworkModelEMeshHopByHopGeneric::computeLatency(OutputDirection direction, SubsecondTime pkt_time, UInt32 pkt_length, core_id_t requester)
{
   LOG_ASSERT_ERROR((direction >= 0) && (direction < NUM_OUTPUT_DIRECTIONS),
         "Invalid Direction(%u)", direction);

   if ( (!m_enabled) || (requester >= (core_id_t) Config::getSingleton()->getApplicationCores()) )
      return SubsecondTime::Zero();

   SubsecondTime processing_time = computeProcessingTime(pkt_length);

   SubsecondTime queue_delay = SubsecondTime::Zero();
   if (m_queue_model_enabled)
      queue_delay = m_queue_models[direction]->computeQueueDelay(pkt_time, processing_time);

   LOG_PRINT("Queue Delay(%s), Hop Latency(%s)", itostr(queue_delay).c_str(), itostr(m_hop_latency.getLatency()).c_str());
   SubsecondTime packet_latency = m_hop_latency.getLatency() + queue_delay;

   return packet_latency;
}

SubsecondTime
NetworkModelEMeshHopByHopGeneric::computeInjectionPortQueueDelay(core_id_t pkt_receiver, SubsecondTime pkt_time, UInt32 pkt_length)
{
   if (!m_queue_model_enabled)
      return SubsecondTime::Zero();

   if (pkt_receiver == m_core_id)
      return SubsecondTime::Zero();

   SubsecondTime processing_time = computeProcessingTime(pkt_length);
   return m_injection_port_queue_model->computeQueueDelay(pkt_time, processing_time);
}

SubsecondTime
NetworkModelEMeshHopByHopGeneric::computeEjectionPortQueueDelay(SubsecondTime pkt_time, UInt32 pkt_length)
{
   if (!m_queue_model_enabled)
      return SubsecondTime::Zero();

   SubsecondTime processing_time = computeProcessingTime(pkt_length);
   return m_ejection_port_queue_model->computeQueueDelay(pkt_time, processing_time);
}

SubsecondTime
NetworkModelEMeshHopByHopGeneric::computeProcessingTime(UInt32 pkt_length)
{
   // Send: (pkt_length * 8) bits
   // Bandwidth: (m_link_bandwidth) bits/cycle
   UInt32 num_bits = pkt_length * 8;
   return m_link_bandwidth.getRoundedLatency(num_bits);
}

SInt32
NetworkModelEMeshHopByHopGeneric::getNextDest(SInt32 final_dest, OutputDirection& direction)
{
   // Do dimension-order routing
   // Curently, do store-and-forward routing
   // FIXME: Should change this to wormhole routing eventually

   SInt32 sx, sy, dx, dy;

   computePosition(m_core_id, sx, sy);
   computePosition(final_dest, dx, dy);

   if (sx > dx)
   {
      direction = LEFT;
      return computeCoreId(sx-1,sy);
   }
   else if (sx < dx)
   {
      direction = RIGHT;
      return computeCoreId(sx+1,sy);
   }
   else if (sy > dy)
   {
      direction = DOWN;
      return computeCoreId(sx,sy-1);
   }
   else if (sy < dy)
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
NetworkModelEMeshHopByHopGeneric::outputSummary(std::ostream &out)
{
   out << "    bytes received: " << m_total_bytes_received << std::endl;
   out << "    packets received: " << m_total_packets_received << std::endl;
   if (m_total_packets_received > 0)
   {
      out << "    average contention delay: " <<
         (m_total_contention_delay / m_total_packets_received) << std::endl;
      out << "    average packet latency: " <<
         (m_total_packet_latency / m_total_packets_received) << std::endl;
   }
   else
   {
      out << "    average contention delay: " <<
         "NA" << std::endl;
      out << "    average packet latency: " <<
         "NA" << std::endl;
   }

   if (m_queue_model_enabled && (m_queue_model_type == "history_list"))
   {
      out << "  Queue Models:" << std::endl;

      float queue_utilization = 0.0;
      float frac_requests_using_analytical_model = 0.0;
      UInt32 num_queue_models = 0;

      for (SInt32 i = 0; i < NUM_OUTPUT_DIRECTIONS; i++)
      {
         if (m_queue_models[i])
         {
            queue_utilization += ((QueueModelHistoryList*) m_queue_models[i])->getQueueUtilization();
            frac_requests_using_analytical_model += ((QueueModelHistoryList*) m_queue_models[i])->getFracRequestsUsingAnalyticalModel();
            num_queue_models ++;
         }
      }

      queue_utilization /= num_queue_models;
      frac_requests_using_analytical_model /= num_queue_models;

      out << "    Queue Utilization(\%): " << queue_utilization * 100 << std::endl;
      out << "    Analytical Model Used(\%): " << frac_requests_using_analytical_model * 100 << std::endl;
   }
}

void
NetworkModelEMeshHopByHopGeneric::enable()
{
   m_enabled = true;
}

void
NetworkModelEMeshHopByHopGeneric::disable()
{
   m_enabled = false;
}

bool
NetworkModelEMeshHopByHopGeneric::isEnabled()
{
   return m_enabled;
}

std::pair<bool,SInt32>
NetworkModelEMeshHopByHopGeneric::computeCoreCountConstraints(SInt32 core_count)
{
   SInt32 mesh_width = (SInt32) floor (sqrt(core_count));
   SInt32 mesh_height = (SInt32) ceil (1.0 * core_count / mesh_width);

   assert(core_count <= mesh_width * mesh_height);
   assert(core_count > (mesh_width - 1) * mesh_height);
   assert(core_count > mesh_width * (mesh_height - 1));

   return std::make_pair(true,mesh_height * mesh_width);
}

std::pair<bool, std::vector<core_id_t> >
NetworkModelEMeshHopByHopGeneric::computeMemoryControllerPositions(SInt32 num_memory_controllers, SInt32 core_count)
{
   SInt32 mesh_width = (SInt32) floor (sqrt(core_count));
   SInt32 mesh_height = (SInt32) ceil (1.0 * core_count / mesh_width);

   assert(core_count == (mesh_height * mesh_width));

   // core_id_list_along_perimeter : list of cores along the perimeter of the chip in clockwise order starting from (0,0)
   std::vector<core_id_t> core_id_list_along_perimeter;

   for (SInt32 i = 0; i < mesh_width; i++)
      core_id_list_along_perimeter.push_back(i);

   for (SInt32 i = 1; i < (mesh_height-1); i++)
      core_id_list_along_perimeter.push_back((i * mesh_width) + mesh_width-1);

   for (SInt32 i = mesh_width-1; i >= 0; i--)
      core_id_list_along_perimeter.push_back(((mesh_height-1) * mesh_width) + i);

   for (SInt32 i = mesh_height-2; i >= 1; i--)
      core_id_list_along_perimeter.push_back(i * mesh_width);

   assert(core_id_list_along_perimeter.size() == (UInt32) (2 * (mesh_width + mesh_height - 2)));

   LOG_ASSERT_ERROR(core_id_list_along_perimeter.size() >= (UInt32) num_memory_controllers,
         "num cores along perimeter(%u), num memory controllers(%i)",
         core_id_list_along_perimeter.size(), num_memory_controllers);

   SInt32 spacing_between_memory_controllers = core_id_list_along_perimeter.size() / num_memory_controllers;

   // core_id_list_with_memory_controllers : list of cores that have memory controllers attached to them
   std::vector<core_id_t> core_id_list_with_memory_controllers;

   for (SInt32 i = 0; i < num_memory_controllers; i++)
   {
      SInt32 index = (i * spacing_between_memory_controllers + mesh_width/2) % core_id_list_along_perimeter.size();
      core_id_list_with_memory_controllers.push_back(core_id_list_along_perimeter[index]);
   }

   return (std::make_pair(true, core_id_list_with_memory_controllers));
}

SInt32
NetworkModelEMeshHopByHopGeneric::computeNumHops(core_id_t sender, core_id_t receiver)
{
   SInt32 core_count = Config::getSingleton()->getTotalCores();

   SInt32 mesh_width = (SInt32) floor (sqrt(core_count));
   // SInt32 mesh_height = (SInt32) ceil (1.0 * core_count / mesh_width);

   SInt32 sx, sy, dx, dy;

   sx = sender % mesh_width;
   sy = sender / mesh_width;
   dx = receiver % mesh_width;
   dy = receiver / mesh_width;

   return abs(sx - dx) + abs(sy - dy);
}
