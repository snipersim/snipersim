#include <math.h>
#include <stdlib.h>

#include "simulator.h"
#include "network_model_analytical.h"
#include "network_model_analytical_params.h"
#include "message_types.h"
#include "config.h"
#include "core.h"
#include "performance_model.h"
#include "transport.h"
#include "lock.h"
#include "log.h"
#include "config.hpp"

#define IS_NAN(x) (!((x < 0.0) || (x >= 0.0)))

#define GET_INT(s) ( Sim()->getCfg()->getInt("network/analytical/" s) )

NetworkModelAnalytical::NetworkModelAnalytical(Network *net, EStaticNetwork net_type)
      : NetworkModel(net)
      , _bytesSent(0)
      , _cyclesProc(SubsecondTime::Zero())
      , _cyclesLatency(SubsecondTime::Zero())
      , _cyclesContention(SubsecondTime::Zero())
      , _globalUtilization(0)
      , _localUtilizationLastUpdate(SubsecondTime::Zero())
      , _localUtilizationFlitsSent(0)
      , m_enabled(false)
{
   getNetwork()->registerCallback(MCP_UTILIZATION_UPDATE_TYPE,
                                  receiveMCPUpdate,
                                  this);

   try
     {
       // Create network parameters
       m_params.Tw2 = GET_INT("Tw2"); // single cycle between nodes in 2d mesh
       m_params.s = GET_INT("s"); // single cycle switching time
       m_params.n = GET_INT("n"); // 2-d mesh network
       m_params.W = GET_INT("W"); // 32-bit wide channels
       m_params.update_interval = SubsecondTime::NS() * GET_INT("update_interval");
       m_params.proc_cost = ((net_type == STATIC_NETWORK_MEMORY_1) || (net_type == STATIC_NETWORK_MEMORY_2)) ? SubsecondTime::Zero() : (SubsecondTime::NS() * GET_INT("processing_cost"));
     }
   catch (...)
     {
       LOG_PRINT_ERROR("Some analytical network parameters not available.");
     }
}

NetworkModelAnalytical::~NetworkModelAnalytical()
{
   getNetwork()->unregisterCallback(MCP_UTILIZATION_UPDATE_TYPE);
}

void NetworkModelAnalytical::routePacket(const NetPacket &pkt,
      std::vector<Hop> &nextHops)
{
   ScopedLock sl(_lock);

   // basic magic network routing, with two additions
   // (1) compute latency of packet
   // (2) update utilization

   PerformanceModel *perf = getNetwork()->getCore()->getPerformanceModel();

   Hop h;
   h.final_dest = pkt.receiver;
   h.next_dest = pkt.receiver;

   SubsecondTime network_latency = computeLatency(pkt);
   h.time = pkt.time + network_latency;

   nextHops.push_back(h);

   if (m_params.proc_cost > SubsecondTime::Zero())
      perf->queueDynamicInstruction(new DynamicInstruction(m_params.proc_cost));
   _cyclesProc += m_params.proc_cost;

   updateUtilization();

   UInt32 pkt_length = getNetwork()->getModeledLength(pkt);
   _bytesSent += pkt_length;
}

SubsecondTime NetworkModelAnalytical::computeLatency(const NetPacket &packet)
{
   if (!m_enabled)
      return SubsecondTime::Zero();

   // self-sends incur no cost
   if (packet.sender == packet.receiver)
     return SubsecondTime::Zero();

   // We model a unidirectional network with end-around connections
   // using the network model in "Limits on Interconnect Performance"
   // (by Anant). Currently, this ignores communication locality and
   // sets static values for all parameters. We also assume uniform
   // packet distribution, so we do not take the packet destination
   // into account.
   //
   // We combine the contention model (eq. 12) with the model for
   // limited bisection width (sec. 4.2).

   // TODO: Fix (if necessary) distance calculation for uneven
   // topologies, i.e. 8-node 2d mesh or 10-node 3d mesh.

   // TODO: Confirm model for latency computed based on actual number
   // of network hops.

   // Retrieve parameters
   double Tw2 = m_params.Tw2;
   double s = m_params.s;
   int n = m_params.n;
   double W = m_params.W;
   double p = _globalUtilization;
   LOG_ASSERT_ERROR(!IS_NAN(_globalUtilization) && 0 <= _globalUtilization && _globalUtilization < 1, "Recv'd invalid global utilization value: %f", _globalUtilization);

   // This lets us derive the latency, ignoring contention

   int N;                    // number of nodes in the network
   double k;                 // length of mesh in one dimension
   double kd;                // number of hops per dimension
   double time_per_hop;      // time spent in one hop through the network
   double B;                 // number of flits for packet
   double hops_in_network;   // number of nodes visited
   double Tb;                // latency, without contention

   N = Config::getSingleton()->getTotalCores();
   k = pow(N, 1./n);                  // pg 5
   kd = k/2.;                         // pg 5 (note this will be
   // different for different network configurations...say,
   // bidirectional networks)
   time_per_hop = s + pow(k, n/2.-1.);  // pg 6

   UInt32 packet_length = getNetwork()->getModeledLength(packet);
   B = ceil(packet_length * 8. / W);

   // Compute the number of hops based on src, dest
   // Based on this eqn:
   // node number = x1 + x2 k + x3 k^2 + ... + xn k^(n-1)
   int network_distance = 0;
   int src = packet.sender;
   int dest = packet.receiver;
   int ki = (int)k;
   for (int i = 0; i < n; i++)
   {
      div_t q1, q2;
      q1 = div(src, ki);
      q2 = div(dest, ki);
      // This models a unidirectional network distance
      // Should use abs() for bidirectional.
      if (q1.rem > q2.rem)
         network_distance += ki - (q1.rem - q2.rem);
      else
         network_distance += q2.rem - q1.rem;
      src = q1.quot;
      dest = q2.quot;
   }
   hops_in_network = network_distance + B; // B flits must be sent
   // (with wormhole routing, this adds B hops)

   Tb = Tw2 * time_per_hop * hops_in_network; // pg 5

   // Now to model contention... (sec 3.2)
   double w;                 // delay due to contention

   w  = p * B / (1. - p);
   w *= (kd-1.)/(kd*kd);
   w *= 1.+1./((double)n);

   if (w < 0) w = 0; // correct negative contention values for small
   // networks

   double hops_with_contention;
   double Tc;                // latency, with contention
   hops_with_contention = network_distance * (1. + w) + B;
   Tc = Tw2 * time_per_hop * hops_with_contention;

   // Computation finished...
   SubsecondTime Tci = SubsecondTime::NS() * ceil(Tc);
   _cyclesLatency += Tci;
   _cyclesContention += SubsecondTime::NS() * static_cast<UInt64>(Tc - Tb);

   // ** update utilization counter **
   // we must account for the usage throughout the mesh
   // which means that we must include the # of hops
   _localUtilizationFlitsSent += (UInt64)(B * hops_in_network);

   return Tci;
}

void NetworkModelAnalytical::outputSummary(std::ostream &out)
{
   out << "    bytes sent: " << _bytesSent << std::endl;
   out << "    cycles spent proc: " << _cyclesProc << std::endl;
   out << "    cycles spent latency: " << _cyclesLatency << std::endl;
   out << "    cycles spent contention: " << _cyclesContention << std::endl;
}

struct UtilizationMessage
{
   double ut;
   NetworkModelAnalytical *model;
};

struct UtilizationMCPMessage
{
   int msg_type;
   UtilizationMessage msg;
};

// we send and receive updates asynchronously for performance and
// because the MCP is unable to reply to itself.

void NetworkModelAnalytical::updateUtilization()
{
   // ** send updates

   // don't lock because this is all approximate anyway
   SubsecondTime core_time = getNetwork()->getCore()->getPerformanceModel()->getElapsedTime();
   SubsecondTime elapsed_time = core_time - _localUtilizationLastUpdate;

   if (elapsed_time < m_params.update_interval)
      return;

   // FIXME: This assumes one cycle per flit, might not be accurate.
   // FIXME: Will this new local utilization be correctly consumed?
   double local_utilization = ((double)_localUtilizationFlitsSent) / ((double)elapsed_time.getInternalDataForced());

   LOG_ASSERT_WARNING(0 <= local_utilization && local_utilization < 1, "Unusual local utilization value: %f", local_utilization);

   _localUtilizationLastUpdate = core_time;
   _localUtilizationFlitsSent = 0;

   // build packet
   UtilizationMCPMessage m;
   m.msg_type = MCP_MESSAGE_UTILIZATION_UPDATE;
   m.msg.ut = local_utilization;
   m.msg.model = this;

   NetPacket update;
   update.sender = getNetwork()->getCore()->getId();
   update.receiver = Config::getSingleton()->getMCPCoreNum();
   update.length = sizeof(m);
   update.type = MCP_SYSTEM_TYPE;
   update.data = &m;

//   getNetwork()->netSend(update);
}

void NetworkModelAnalytical::receiveMCPUpdate(void *obj, NetPacket response)
{
   assert(response.length == sizeof(UtilizationMessage));

   UtilizationMessage *pr = (UtilizationMessage*) response.data;

   ScopedLock sl(pr->model->_lock);

   LOG_ASSERT_ERROR(!IS_NAN(pr->ut) && 0 <= pr->ut && pr->ut < 1, "Recv'd invalid global utilization value: %f", pr->ut);
//   fprintf(stderr, "Recv'd global utilization: %f\n", pr->ut);

   pr->model->_globalUtilization = pr->ut;
}

void NetworkModelAnalytical::enable()
{
   m_enabled = true;
}

void NetworkModelAnalytical::disable()
{
   m_enabled = false;
}
