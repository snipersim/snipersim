#include "network_model_emesh_hop_by_hop_basic.h"
#include "simulator.h"
#include "config.h"
#include "dvfs_manager.h"
#include "config.hpp"

NetworkModelEMeshHopByHopBasic::NetworkModelEMeshHopByHopBasic(Network* net):
   NetworkModelEMeshHopByHopGeneric(net)
{
   m_broadcast_tree_enabled = false;

   // Get the Link Bandwidth, Hop Latency and if it has broadcast tree mechanism
   try
   {
      // Link Bandwidth is specified in bits/clock_cycle
      m_link_bandwidth = ComponentBandwidthPerCycle(Sim()->getDvfsManager()->getCoreDomain(m_core_id), Sim()->getCfg()->getInt("network/emesh_hop_by_hop_basic/link_bandwidth"));
      // Hop Latency is specified in cycles
      m_hop_latency = ComponentLatency(Sim()->getDvfsManager()->getCoreDomain(m_core_id), Sim()->getCfg()->getInt("network/emesh_hop_by_hop_basic/hop_latency"));

      // Queue Model enabled? If no, this degrades into a hop counter model
      m_queue_model_enabled = Sim()->getCfg()->getBool("network/emesh_hop_by_hop_basic/queue_model/enabled");
      m_queue_model_type = Sim()->getCfg()->getString("network/emesh_hop_by_hop_basic/queue_model/type");
   }
   catch(...)
   {
      LOG_PRINT_ERROR("Could not read parameters from the configuration file");
   }

   createQueueModels();
}
