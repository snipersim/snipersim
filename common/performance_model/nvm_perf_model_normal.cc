#include "nvm_perf_model_normal.h"
#include "simulator.h"
#include "config.h"
#include "config.hpp"
#include "stats.h"
#include "shmem_perf.h"

NvmPerfModelNormal::NvmPerfModelNormal(core_id_t core_id, UInt32 cache_block_size) :
      NvmPerfModel(core_id, cache_block_size),
      m_queue_model(nullptr),
      m_nvm_bandwidth(8 * Sim()->getCfg()->getFloat("perf_model/dram/per_controller_bandwidth")),
      m_total_queueing_delay(SubsecondTime::Zero()),
      m_total_access_latency(SubsecondTime::Zero())
{
   SubsecondTime nvm_latency_stddev = SubsecondTime::FS() * static_cast<uint64_t>(TimeConverter<float>::NStoFS(
         Sim()->getCfg()->getFloat("perf_model/dram/normal/standard_deviation")));

   // Operate in fs for higher precision before converting to uint64_t/SubsecondTime
   SubsecondTime nvm_read_latency = NvmPerfModel::getReadLatency();
   m_nvm_read_cost = new NormalTimeDistribution(nvm_read_latency, nvm_latency_stddev);

   SubsecondTime nvm_write_latency = NvmPerfModel::getWriteLatency();
   m_nvm_write_cost = new NormalTimeDistribution(nvm_write_latency, nvm_latency_stddev);

   SubsecondTime nvm_log_latency = NvmPerfModel::getLogLatency();
   m_nvm_log_cost = new NormalTimeDistribution(nvm_log_latency, nvm_latency_stddev);

   if (Sim()->getCfg()->getBool("perf_model/dram/queue_model/enabled")) {
      m_queue_model = QueueModel::create("dram-queue", core_id,
                                         Sim()->getCfg()->getString("perf_model/dram/queue_model/type"),
                                         m_nvm_bandwidth.getRoundedLatency(8 * cache_block_size)); // bytes to bits
   }

   registerStatsMetric("dram", core_id, "total-access-latency", &m_total_access_latency);
   registerStatsMetric("dram", core_id, "total-queueing-delay", &m_total_queueing_delay);
}

NvmPerfModelNormal::~NvmPerfModelNormal()
{
   if (m_queue_model) {
      delete m_queue_model;
      m_queue_model = nullptr;
   }
   delete m_nvm_read_cost;
   delete m_nvm_write_cost;
   delete m_nvm_log_cost;
}

SubsecondTime
NvmPerfModelNormal::getAccessLatency(SubsecondTime pkt_time, UInt64 pkt_size, core_id_t requester, IntPtr address,
                                     DramCntlrInterface::access_t access_type, ShmemPerf *perf)
{
   // pkt_size is in 'Bytes'
   // m_dram_bandwidth is in 'Bits per clock cycle'
   if ((!m_enabled) || (requester >= (core_id_t) Config::getSingleton()->getApplicationCores())) {
      return SubsecondTime::Zero();
   }

   SubsecondTime processing_time = m_nvm_bandwidth.getRoundedLatency(8 * pkt_size); // bytes to bits

   // Compute Queue Delay
   SubsecondTime queue_delay = m_queue_model ? m_queue_model->computeQueueDelay(pkt_time, processing_time, requester)
                                             : SubsecondTime::Zero();

   SubsecondTime nvm_access_cost = (access_type == DramCntlrInterface::WRITE) ? m_nvm_write_cost->next()
                                                                              : m_nvm_read_cost->next();
   SubsecondTime access_latency = queue_delay + processing_time + nvm_access_cost;


   perf->updateTime(pkt_time);
   perf->updateTime(pkt_time + queue_delay, ShmemPerf::DRAM_QUEUE);
   perf->updateTime(pkt_time + queue_delay + processing_time, ShmemPerf::DRAM_BUS);
   perf->updateTime(pkt_time + queue_delay + processing_time + nvm_access_cost, ShmemPerf::DRAM_DEVICE);

   // Update Memory Counters
   m_num_accesses++;
   m_total_access_latency += access_latency;
   m_total_queueing_delay += queue_delay;

   return access_latency;
}
