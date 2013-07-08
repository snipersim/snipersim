#include "dram_perf_model_normal.h"
#include "simulator.h"
#include "config.h"
#include "config.hpp"
#include "stats.h"
#include "shmem_perf.h"

DramPerfModelNormal::DramPerfModelNormal(core_id_t core_id,
      UInt32 cache_block_size):
   DramPerfModel(core_id, cache_block_size),
   m_queue_model(NULL),
   m_dram_bandwidth(8 * Sim()->getCfg()->getFloat("perf_model/dram/per_controller_bandwidth")), // Convert bytes to bits
   m_total_queueing_delay(SubsecondTime::Zero()),
   m_total_access_latency(SubsecondTime::Zero())
{
   SubsecondTime dram_latency = SubsecondTime::FS() * static_cast<uint64_t>(TimeConverter<float>::NStoFS(Sim()->getCfg()->getFloat("perf_model/dram/latency"))); // Operate in fs for higher precision before converting to uint64_t/SubsecondTime
   SubsecondTime dram_latency_stddev = SubsecondTime::FS() * static_cast<uint64_t>(TimeConverter<float>::NStoFS(Sim()->getCfg()->getFloat("perf_model/dram/normal/standard_deviation")));

   m_dram_access_cost = new NormalTimeDistribution(dram_latency, dram_latency_stddev);

   if (Sim()->getCfg()->getBool("perf_model/dram/queue_model/enabled"))
   {
      m_queue_model = QueueModel::create("dram-queue", core_id, Sim()->getCfg()->getString("perf_model/dram/queue_model/type"),
                                         m_dram_bandwidth.getRoundedLatency(8 * cache_block_size)); // bytes to bits
   }

   registerStatsMetric("dram", core_id, "total-access-latency", &m_total_access_latency);
   registerStatsMetric("dram", core_id, "total-queueing-delay", &m_total_queueing_delay);
}

DramPerfModelNormal::~DramPerfModelNormal()
{
   if (m_queue_model)
   {
     delete m_queue_model;
      m_queue_model = NULL;
   }
   delete m_dram_access_cost;
}

SubsecondTime
DramPerfModelNormal::getAccessLatency(SubsecondTime pkt_time, UInt64 pkt_size, core_id_t requester, IntPtr address, DramCntlrInterface::access_t access_type, ShmemPerf *perf)
{
   // pkt_size is in 'Bytes'
   // m_dram_bandwidth is in 'Bits per clock cycle'
   if ((!m_enabled) ||
         (requester >= (core_id_t) Config::getSingleton()->getApplicationCores()))
   {
      return SubsecondTime::Zero();
   }

   SubsecondTime processing_time = m_dram_bandwidth.getRoundedLatency(8 * pkt_size); // bytes to bits

   // Compute Queue Delay
   SubsecondTime queue_delay;
   if (m_queue_model)
   {
      queue_delay = m_queue_model->computeQueueDelay(pkt_time, processing_time, requester);
   }
   else
   {
      queue_delay = SubsecondTime::Zero();
   }

   SubsecondTime dram_access_cost = m_dram_access_cost->next();
   SubsecondTime access_latency = queue_delay + processing_time + dram_access_cost;


   perf->updateTime(pkt_time);
   perf->updateTime(pkt_time + queue_delay, ShmemPerf::DRAM_QUEUE);
   perf->updateTime(pkt_time + queue_delay + processing_time, ShmemPerf::DRAM_BUS);
   perf->updateTime(pkt_time + queue_delay + processing_time + dram_access_cost, ShmemPerf::DRAM_DEVICE);

   // Update Memory Counters
   m_num_accesses ++;
   m_total_access_latency += access_latency;
   m_total_queueing_delay += queue_delay;

   return access_latency;
}
