#include "dram_cache.h"
#include "simulator.h"
#include "config.hpp"
#include "cache.h"
#include "stats.h"
#include "memory_manager_base.h"
#include "pr_l1_cache_block_info.h"
#include "queue_model.h"
#include "shmem_perf.h"
#include "prefetcher.h"

DramCache::DramCache(MemoryManagerBase* memory_manager, ShmemPerfModel* shmem_perf_model, AddressHomeLookup* home_lookup, UInt32 cache_block_size, DramCntlrInterface *dram_cntlr)
   : DramCntlrInterface(memory_manager, shmem_perf_model, cache_block_size)
   , m_core_id(memory_manager->getCore()->getId())
   , m_cache_block_size(cache_block_size)
   , m_data_access_time(SubsecondTime::NS(Sim()->getCfg()->getIntArray("perf_model/dram/cache/data_access_time", m_core_id)))
   , m_tags_access_time(SubsecondTime::NS(Sim()->getCfg()->getIntArray("perf_model/dram/cache/tags_access_time", m_core_id)))
   , m_data_array_bandwidth(8 * Sim()->getCfg()->getFloat("perf_model/dram/cache/bandwidth"))
   , m_home_lookup(home_lookup)
   , m_dram_cntlr(dram_cntlr)
   , m_queue_model(NULL)
   , m_prefetcher(NULL)
   , m_prefetch_mshr("dram-cache.prefetch-mshr", m_core_id, 16)
   , m_reads(0)
   , m_writes(0)
   , m_read_misses(0)
   , m_write_misses(0)
   , m_hits_prefetch(0)
   , m_prefetches(0)
   , m_prefetch_mshr_delay(SubsecondTime::Zero())
{
   UInt32 cache_size = Sim()->getCfg()->getIntArray("perf_model/dram/cache/cache_size", m_core_id);
   UInt32 associativity = Sim()->getCfg()->getIntArray("perf_model/dram/cache/associativity", m_core_id);
   UInt32 num_sets = k_KILO * cache_size / (associativity * m_cache_block_size);
   LOG_ASSERT_ERROR(k_KILO * cache_size == num_sets * associativity * m_cache_block_size, "Invalid cache configuration: size(%d Kb) != sets(%d) * associativity(%d) * block_size(%d)", cache_size, num_sets, associativity, m_cache_block_size);

   m_cache = new Cache("dram-cache",
      "perf_model/dram/cache",
      m_core_id,
      num_sets,
      associativity,
      m_cache_block_size,
      Sim()->getCfg()->getStringArray("perf_model/dram/cache/replacement_policy", m_core_id),
      CacheBase::PR_L1_CACHE,
      CacheBase::parseAddressHash(Sim()->getCfg()->getStringArray("perf_model/dram/cache/address_hash", m_core_id)),
      NULL, /* FaultinjectionManager */
      home_lookup
   );

   if (Sim()->getCfg()->getBool("perf_model/dram/cache/queue_model/enabled"))
   {
      String queue_model_type = Sim()->getCfg()->getString("perf_model/dram/queue_model/type");
      m_queue_model = QueueModel::create("dram-cache-queue", m_core_id, queue_model_type, m_data_array_bandwidth.getRoundedLatency(8 * m_cache_block_size)); // bytes to bits
   }

   m_prefetcher = Prefetcher::createPrefetcher(Sim()->getCfg()->getString("perf_model/dram/cache/prefetcher"), "dram/cache", m_core_id, 1);
   m_prefetch_on_prefetch_hit = Sim()->getCfg()->getBool("perf_model/dram/cache/prefetcher/prefetch_on_prefetch_hit");

   registerStatsMetric("dram-cache", m_core_id, "reads", &m_reads);
   registerStatsMetric("dram-cache", m_core_id, "writes", &m_writes);
   registerStatsMetric("dram-cache", m_core_id, "read-misses", &m_read_misses);
   registerStatsMetric("dram-cache", m_core_id, "write-misses", &m_write_misses);
   registerStatsMetric("dram-cache", m_core_id, "hits-prefetch", &m_hits_prefetch);
   registerStatsMetric("dram-cache", m_core_id, "prefetches", &m_prefetches);
   registerStatsMetric("dram-cache", m_core_id, "prefetch-mshr-delay", &m_prefetch_mshr_delay);
}

DramCache::~DramCache()
{
   delete m_cache;
   if (m_queue_model)
      delete m_queue_model;
}

boost::tuple<SubsecondTime, HitWhere::where_t>
DramCache::getDataFromDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now, ShmemPerf *perf)
{
   std::pair<bool, SubsecondTime> res = doAccess(Cache::LOAD, address, requester, data_buf, now, perf);

   if (!res.first)
      ++m_read_misses;
   ++m_reads;

   return boost::tuple<SubsecondTime, HitWhere::where_t>(res.second, res.first ? HitWhere::DRAM_CACHE : HitWhere::DRAM);
}

boost::tuple<SubsecondTime, HitWhere::where_t>
DramCache::putDataToDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now)
{
   std::pair<bool, SubsecondTime> res = doAccess(Cache::STORE, address, requester, data_buf, now, NULL);

   if (!res.first)
      ++m_write_misses;
   ++m_writes;

   return boost::tuple<SubsecondTime, HitWhere::where_t>(res.second, res.first ? HitWhere::DRAM_CACHE : HitWhere::DRAM);
}

std::pair<bool, SubsecondTime>
DramCache::doAccess(Cache::access_t access, IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now, ShmemPerf *perf)
{
   PrL1CacheBlockInfo* block_info = (PrL1CacheBlockInfo*)m_cache->peekSingleLine(address);
   SubsecondTime latency = m_tags_access_time;
   perf->updateTime(now);
   perf->updateTime(now + latency, ShmemPerf::DRAM_CACHE_TAGS);
   bool cache_hit = false, prefetch_hit = false;

   if (block_info)
   {
      cache_hit = true;

      if (block_info->hasOption(CacheBlockInfo::PREFETCH))
      {
         // This line was fetched by the prefetcher and has proven useful
         m_hits_prefetch++;
         prefetch_hit = true;
         block_info->clearOption(CacheBlockInfo::PREFETCH);

         // If prefetch is still in progress: delay
         SubsecondTime t_completed = m_prefetch_mshr.getTagCompletionTime(address);
         if (t_completed != SubsecondTime::MaxTime() && t_completed > now + latency)
         {
            m_prefetch_mshr_delay += t_completed - (now + latency);
            latency = t_completed - now;
         }
      }

      m_cache->accessSingleLine(address, access, data_buf, m_cache_block_size, now + latency, true);

      latency += accessDataArray(access, requester, now + latency, perf);
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
         boost::tie(dram_latency, hit_where) = m_dram_cntlr->getDataFromDram(address, requester, data_buf, now + latency, perf);
         latency += dram_latency;
      }
         // For STOREs, we only do complete cache lines so we don't need to read from DRAM

      insertLine(access, address, requester, data_buf, now + latency);
   }

   if (m_prefetcher)
      callPrefetcher(address, cache_hit, prefetch_hit, now + latency);

   return std::pair<bool, SubsecondTime>(block_info ? true : false, latency);
}

void
DramCache::insertLine(Cache::access_t access, IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now)
{
   bool eviction;
   IntPtr evict_address;
   PrL1CacheBlockInfo evict_block_info;
   Byte evict_buf[m_cache_block_size];

   m_cache->insertSingleLine(address, data_buf,
      &eviction, &evict_address, &evict_block_info, evict_buf,
      now);
   m_cache->peekSingleLine(address)->setCState(access == Cache::STORE ? CacheState::MODIFIED : CacheState::SHARED);

   // Write to data array off-line, so don't affect return latency
   accessDataArray(Cache::STORE, requester, now, NULL);

   // Writeback to DRAM done off-line, so don't affect return latency
   if (eviction && evict_block_info.getCState() == CacheState::MODIFIED)
   {
      m_dram_cntlr->putDataToDram(evict_address, requester, data_buf, now);
   }
}

SubsecondTime
DramCache::accessDataArray(Cache::access_t access, core_id_t requester, SubsecondTime t_start, ShmemPerf *perf)
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

   perf->updateTime(t_start);
   perf->updateTime(t_start + queue_delay, ShmemPerf::DRAM_CACHE_QUEUE);
   perf->updateTime(t_start + queue_delay + processing_time, ShmemPerf::DRAM_CACHE_BUS);
   perf->updateTime(t_start + queue_delay + processing_time + m_data_access_time, ShmemPerf::DRAM_CACHE_DATA);

   return queue_delay + processing_time + m_data_access_time;
}

void
DramCache::callPrefetcher(IntPtr train_address, bool cache_hit, bool prefetch_hit, SubsecondTime t_issue)
{
   // Always train the prefetcher
   std::vector<IntPtr> prefetchList = m_prefetcher->getNextAddress(train_address, INVALID_CORE_ID);

   // Only do prefetches on misses, or on hits to lines previously brought in by the prefetcher (if enabled)
   if (!cache_hit || (m_prefetch_on_prefetch_hit && prefetch_hit))
   {
      for(std::vector<IntPtr>::iterator it = prefetchList.begin(); it != prefetchList.end(); ++it)
      {
         IntPtr prefetch_address = *it;
         if (!m_cache->peekSingleLine(prefetch_address))
         {
            // Get data from DRAM
            SubsecondTime dram_latency;
            HitWhere::where_t hit_where;
            Byte data_buf[m_cache_block_size];
            boost::tie(dram_latency, hit_where) = m_dram_cntlr->getDataFromDram(prefetch_address, m_core_id, data_buf, t_issue, NULL);
            // Insert into data array
            insertLine(Cache::LOAD, prefetch_address, m_core_id, data_buf, t_issue + dram_latency);
            // Set prefetched bit
            PrL1CacheBlockInfo* block_info = (PrL1CacheBlockInfo*)m_cache->peekSingleLine(prefetch_address);
            block_info->setOption(CacheBlockInfo::PREFETCH);
            // Update completion time
            m_prefetch_mshr.getCompletionTime(t_issue, dram_latency, prefetch_address);

            ++m_prefetches;
         }
      }
   }
}
