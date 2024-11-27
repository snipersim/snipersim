#include "nvm_perf_model_readwrite.h"
#include "config.hpp"
#include "shmem_perf.h"
#include "simulator.h"
#include "stats.h"

NvmPerfModelReadWrite::NvmPerfModelReadWrite(core_id_t core_id, UInt32 cache_block_size) :
    NvmPerfModel(core_id, cache_block_size),
    m_queue_model_read(nullptr),
    m_queue_model_write(nullptr),
    m_nvm_read_cost(NvmPerfModel::getReadLatency()),
    m_nvm_write_cost(NvmPerfModel::getWriteLatency()),
    m_shared_readwrite(Sim()->getCfg()->getBool("perf_model/dram/readwrite/shared")),
    m_total_read_queueing_delay(SubsecondTime::Zero()),
    m_total_write_queueing_delay(SubsecondTime::Zero())
{
   if (Sim()->getCfg()->getBool("perf_model/dram/queue_model/enabled"))
   {
      String queue_model_type       = Sim()->getCfg()->getString("perf_model/dram/queue_model/type");
      SubsecondTime rounded_latency = m_nvm_bandwidth.getRoundedLatency(8 * cache_block_size);// bytes to bits

      m_queue_model_read  = QueueModel::create("dram-queue-read", core_id, queue_model_type, rounded_latency);
      m_queue_model_write = QueueModel::create("dram-queue-write", core_id, queue_model_type, rounded_latency);
   }

   registerStatsMetric("dram", core_id, "total-access-latency", &m_total_access_latency);
   registerStatsMetric("dram", core_id, "total-read-queueing-delay", &m_total_read_queueing_delay);
   registerStatsMetric("dram", core_id, "total-write-queueing-delay", &m_total_write_queueing_delay);
}

NvmPerfModelReadWrite::~NvmPerfModelReadWrite()
{
   if (m_queue_model_read)
   {
      delete m_queue_model_read;
      m_queue_model_read = nullptr;
      delete m_queue_model_write;
      m_queue_model_write = nullptr;
   }
}

SubsecondTime
NvmPerfModelReadWrite::computeQueueDelay(SubsecondTime pkt_time, SubsecondTime processing_time, core_id_t requester,
                                         DramCntlrInterface::access_t access_type)
{
   SubsecondTime queue_delay = SubsecondTime::Zero();
   if (m_queue_model_read)
   {
      if (access_type == DramCntlrInterface::READ)
      {
         queue_delay = m_queue_model_read->computeQueueDelay(pkt_time, processing_time, requester);
         if (m_shared_readwrite)
         {
            // Shared read-write bandwidth, but reads are prioritized over writes.
            // With fluffy time, where we can't delay a write because of an earlier (in simulated time) read
            // that was simulated later (in wallclock time), we model this in the following way:
            // - reads are only delayed by other reads (through m_queue_model_read), this assumes *all* writes
            //   can be moved out of the way if needed.
            // - writes see contention by both reads and other writes, i.e., m_queue_model_write
            //   is updated on both read and write.
            m_queue_model_write->computeQueueDelay(pkt_time, processing_time, requester);
         }
      }
      else
      {
         queue_delay = m_queue_model_write->computeQueueDelay(pkt_time, processing_time, requester);
      }
   }
   return queue_delay;
}

void
NvmPerfModelReadWrite::increaseQueueDelay(DramCntlrInterface::access_t access_type, SubsecondTime queue_delay)
{
   if (access_type == DramCntlrInterface::READ)
      m_total_read_queueing_delay += queue_delay;
   else
      m_total_write_queueing_delay += queue_delay;
}
