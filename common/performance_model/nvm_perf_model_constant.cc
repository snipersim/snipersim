#include "nvm_perf_model_constant.h"
#include "config.hpp"
#include "shmem_perf.h"
#include "simulator.h"
#include "stats.h"

NvmPerfModelConstant::NvmPerfModelConstant(core_id_t core_id, UInt32 cache_block_size) :
    NvmPerfModel(core_id, cache_block_size),
    m_queue_model(nullptr),
    m_nvm_read_cost(NvmPerfModel::getReadLatency()),
    m_nvm_write_cost(NvmPerfModel::getWriteLatency()),
    m_total_queueing_delay(SubsecondTime::Zero())
{
   if (Sim()->getCfg()->getBool("perf_model/dram/queue_model/enabled"))
   {
      m_queue_model = QueueModel::create("dram-queue", core_id,
                                         Sim()->getCfg()->getString("perf_model/dram/queue_model/type"),
                                         m_nvm_bandwidth.getRoundedLatency(8 * cache_block_size)); // bytes to bits
   }

   registerStatsMetric("dram", core_id, "total-access-latency", &m_total_access_latency);
   registerStatsMetric("dram", core_id, "total-queueing-delay", &m_total_queueing_delay);
}

NvmPerfModelConstant::~NvmPerfModelConstant()
{
   if (m_queue_model != nullptr)
   {
      delete m_queue_model;
      m_queue_model = nullptr;
   }
}

SubsecondTime
NvmPerfModelConstant::computeQueueDelay(SubsecondTime pkt_time, SubsecondTime processing_time, core_id_t requester,
                                        DramCntlrInterface::access_t access_type)
{
   return (m_queue_model != nullptr) ? m_queue_model->computeQueueDelay(pkt_time, processing_time, requester) : SubsecondTime::Zero();
}

void
NvmPerfModelConstant::increaseQueueDelay(DramCntlrInterface::access_t access_type, SubsecondTime queue_delay)
{
   m_total_queueing_delay += queue_delay;
}