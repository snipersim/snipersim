#include "dram_cache.h"
#include "simulator.h"
#include "config.hpp"
#include "cache.h"
#include "stats.h"
#include "pr_l1_cache_block_info.h"

DramCache::DramCache(core_id_t core_id, UInt32 cache_block_size, DramCntlrInterface *dram_cntlr)
   : m_cache_block_size(cache_block_size)
   , m_data_access_time(SubsecondTime::NS(Sim()->getCfg()->getIntArray("perf_model/dram/cache/tags_access_time", core_id)))
   , m_tags_access_time(SubsecondTime::NS(Sim()->getCfg()->getIntArray("perf_model/dram/cache/data_access_time", core_id)))
   , m_data_array_bandwidth(8 * Sim()->getCfg()->getFloat("perf_model/dram/cache/bandwidth"))
   , m_dram_cntlr(dram_cntlr)
   , m_queue_model(NULL)
   , m_reads(0)
   , m_writes(0)
   , m_read_misses(0)
   , m_write_misses(0)
{
   m_cache = new Cache("dram-cache",
      Sim()->getCfg()->getIntArray("perf_model/dram/cache/cache_size", core_id),
      Sim()->getCfg()->getIntArray("perf_model/dram/cache/associativity", core_id),
      m_cache_block_size,
      Sim()->getCfg()->getStringArray("perf_model/dram/cache/replacement_policy", core_id),
      CacheBase::PR_L1_CACHE,
      CacheBase::parseAddressHash(Sim()->getCfg()->getStringArray("perf_model/dram/cache/address_hash", core_id)),
      NULL /* FaultinjectionManager */
   );

   if (Sim()->getCfg()->getBool("perf_model/dram/cache/queue_model/enabled"))
   {
      String queue_model_type = Sim()->getCfg()->getString("perf_model/dram/queue_model/type");
      m_queue_model = QueueModel::create("dram-cache-queue", core_id, queue_model_type, m_data_array_bandwidth.getRoundedLatency(8 * m_cache_block_size)); // bytes to bits
   }

   registerStatsMetric("dram-cache", core_id, "reads", &m_reads);
   registerStatsMetric("dram-cache", core_id, "writes", &m_writes);
   registerStatsMetric("dram-cache", core_id, "read-misses", &m_read_misses);
   registerStatsMetric("dram-cache", core_id, "write-misses", &m_write_misses);
}

DramCache::~DramCache()
{
   delete m_cache;
   if (m_queue_model)
      delete m_queue_model;
}

boost::tuple<SubsecondTime, HitWhere::where_t>
DramCache::getDataFromDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now)
{
   std::pair<bool, SubsecondTime> res = doAccess(Cache::LOAD, address, requester, data_buf, now);

   if (!res.first)
      ++m_read_misses;
   ++m_reads;

   return boost::tuple<SubsecondTime, HitWhere::where_t>(res.second, res.first ? HitWhere::DRAM_CACHE : HitWhere::DRAM);
}

boost::tuple<SubsecondTime, HitWhere::where_t>
DramCache::putDataToDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now)
{
   std::pair<bool, SubsecondTime> res = doAccess(Cache::STORE, address, requester, data_buf, now);

   if (!res.first)
      ++m_write_misses;
   ++m_writes;

   return boost::tuple<SubsecondTime, HitWhere::where_t>(res.second, res.first ? HitWhere::DRAM_CACHE : HitWhere::DRAM);
}

std::pair<bool, SubsecondTime>
DramCache::doAccess(Cache::access_t access, IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now)
{
   PrL1CacheBlockInfo* block_info = (PrL1CacheBlockInfo*)m_cache->peekSingleLine(address);
   SubsecondTime latency = m_tags_access_time;

   if (block_info)
   {
      latency += accessDataArray(access, requester, now);
      if (access == Cache::STORE)
         block_info->setCState(CacheState::MODIFIED);
   }
   else
   {
      if (access == Cache::LOAD)
      {
         // For LOADs, get data from DRAM
         SubsecondTime dram_latency;
         HitWhere::where_t hit_where;
         boost::tie(dram_latency, hit_where) = m_dram_cntlr->getDataFromDram(address, requester, data_buf, now);
         latency += dram_latency;
      }
         // For STOREs, we only do complete cache lines so we don't need to read from DRAM

      bool eviction;
      IntPtr evict_address;
      PrL1CacheBlockInfo evict_block_info;
      Byte evict_buf[m_cache_block_size];

      m_cache->insertSingleLine(address, data_buf,
         &eviction, &evict_address, &evict_block_info, evict_buf,
         now);
      m_cache->peekSingleLine(address)->setCState(access == Cache::STORE ? CacheState::MODIFIED : CacheState::SHARED);

      // Write to data array off-line, so don't affect return latency
      accessDataArray(Cache::STORE, requester, now + latency);

      // Writeback to DRAM done off-line, so don't affect return latency
      if (eviction && evict_block_info.getCState() == CacheState::MODIFIED)
      {
         m_dram_cntlr->putDataToDram(evict_address, requester, data_buf, now + latency);
      }
   }

   return std::pair<bool, SubsecondTime>(block_info ? true : false, latency);
}

SubsecondTime
DramCache::accessDataArray(Cache::access_t access, core_id_t requester, SubsecondTime t_start)
{
   SubsecondTime processing_time = m_data_array_bandwidth.getRoundedLatency(8 * m_cache_block_size); // bytes to bits

   // Compute Queue Delay
   SubsecondTime queue_delay;
   if (m_queue_model)
   {
      queue_delay = m_queue_model->computeQueueDelay(t_start, processing_time, requester);
   }
   else
   {
      queue_delay = SubsecondTime::Zero();
   }

   return queue_delay + processing_time + m_data_access_time;
}
