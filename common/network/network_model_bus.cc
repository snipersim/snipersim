#include "core_manager.h"
#include "simulator.h"
#include "network.h"
#include "network_model_bus.h"
#include "memory_manager_base.h"
#include "stats.h"
#include "log.h"
#include "dvfs_manager.h"
#include "config.hpp"

NetworkModelBusGlobal* NetworkModelBus::_bus_global[NUM_STATIC_NETWORKS] = { NULL };

NetworkModelBusGlobal::NetworkModelBusGlobal(String name)
   : _bandwidth(8 * Sim()->getCfg()->getFloat("network/bus/bandwidth")) /* = 8 * GB/s / Gcycles/s = bits / cycle, round down (implicit: float to int conversion) */
   , _num_packets(0)
   , _num_packets_delayed(0)
   , _num_bytes(0)
   , _time_used(SubsecondTime::Zero())
   , _total_delay(SubsecondTime::Zero())
{
   String model_type = Sim()->getCfg()->getString("network/bus/queue_model/type");
   // Emulate the original code, with 10 cycles of latency for the history_list, and 0 outstanding transactions for the contention model
   SubsecondTime proc_period = ComponentPeriod::fromFreqHz(Sim()->getCfg()->getFloatArray("perf_model/core/frequency", 0)*1000000000);
   _queue_model = QueueModel::create("bus-queue", 0, model_type, 10 * proc_period);
   /* 8 * GB/s / Gcycles/s = bits / cycle, round down (implicit: float to int conversion) */
   registerStatsMetric(name, 0, "num-packets", &_num_packets);
   registerStatsMetric(name, 0, "num-packets-delayed", &_num_packets_delayed);
   registerStatsMetric(name, 0, "num-bytes", &_num_bytes);
   registerStatsMetric(name, 0, "time-used", &_time_used);
   registerStatsMetric(name, 0, "total-delay", &_total_delay);
}

NetworkModelBusGlobal::~NetworkModelBusGlobal()
{
   delete _queue_model;
}

/* Model bus utilization. In: packet start time and size. Out: packet out time */
SubsecondTime
NetworkModelBusGlobal::useBus(SubsecondTime t_start, UInt32 length, subsecond_time_t *queue_delay_stats)
{
   SubsecondTime t_delay = _bandwidth.getLatency(length * 8);
   SubsecondTime t_queue = _queue_model->computeQueueDelay(t_start, t_delay);
   _time_used += t_delay;
   _total_delay += t_queue;
   if (queue_delay_stats)
      *queue_delay_stats += t_queue;
   if (t_queue > SubsecondTime::Zero())
      _num_packets_delayed ++;
   return t_start + t_queue + t_delay;
}

NetworkModelBus::NetworkModelBus(Network *net, EStaticNetwork net_type)
   : NetworkModel(net, net_type)
   , _enabled(false)
   , _ignore_local(Sim()->getCfg()->getBool("network/bus/ignore_local_traffic"))
{
   if (!_bus_global[net_type]) {
      String name = String("network.")+EStaticNetworkStrings[net_type]+".bus";
      _bus_global[net_type] = new NetworkModelBusGlobal(name);
   }
   _bus = _bus_global[net_type];
}

void
NetworkModelBus::routePacket(const NetPacket &pkt, std::vector<Hop> &nextHops)
{
   SubsecondTime t_recv;
   if (accountPacket(pkt)) {
      ScopedLock sl(_bus->_lock);
      _bus->_num_packets ++;
      _bus->_num_bytes += getNetwork()->getModeledLength(pkt);
      t_recv = _bus->useBus(pkt.time, pkt.length, (subsecond_time_t*)&pkt.queue_delay);
   } else
      t_recv = pkt.time;

   if (pkt.receiver == NetPacket::BROADCAST)
   {
      UInt32 total_cores = Config::getSingleton()->getTotalCores();

      for (SInt32 i = 0; i < (SInt32) total_cores; i++)
      {
         Hop h;
         h.final_dest = i;
         h.next_dest = i;
         h.time = t_recv;

         nextHops.push_back(h);
      }
   }
   else
   {
      Hop h;
      h.final_dest = pkt.receiver;
      h.next_dest = pkt.receiver;
      h.time = t_recv;

      nextHops.push_back(h);
   }
}

void
NetworkModelBus::processReceivedPacket(NetPacket &pkt)
{
}

bool
NetworkModelBus::accountPacket(const NetPacket &pkt)
{
   core_id_t requester = INVALID_CORE_ID;

   if (pkt.type == SHARED_MEM_1)
      requester = getNetwork()->getCore()->getMemoryManager()->getShmemRequester(pkt.data);
   else // Other Packet types
      requester = pkt.sender;

   LOG_ASSERT_ERROR((requester >= 0) && (requester < (core_id_t) Config::getSingleton()->getTotalCores()),
         "requester(%i)", requester);

   if (  !_enabled
         || (_ignore_local && pkt.sender == pkt.receiver)
            // Data to/from MCP: admin traffic, don't account
         || (requester >= (core_id_t) Config::getSingleton()->getApplicationCores())
         || (pkt.sender >= (core_id_t) Config::getSingleton()->getApplicationCores())
         || (pkt.receiver >= (core_id_t) Config::getSingleton()->getApplicationCores())
      )
      return false;
   else
      return true;
}
