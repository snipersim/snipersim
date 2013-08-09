#include "nuca_cache.h"
#include "memory_manager_base.h"
#include "pr_l1_cache_block_info.h"
#include "config.hpp"
#include "stats.h"
#include "queue_model.h"
#include "shmem_perf.h"

NucaCache::NucaCache(MemoryManagerBase* memory_manager, ShmemPerfModel* shmem_perf_model, AddressHomeLookup* home_lookup, UInt32 cache_block_size, ParametricDramDirectoryMSI::CacheParameters& parameters)
   : m_core_id(memory_manager->getCore()->getId())
   , m_memory_manager(memory_manager)
   , m_shmem_perf_model(shmem_perf_model)
   , m_home_lookup(home_lookup)
   , m_cache_block_size(cache_block_size)
   , m_data_access_time(parameters.data_access_time)
   , m_tags_access_time(parameters.tags_access_time)
   , m_data_array_bandwidth(8 * Sim()->getCfg()->getFloat("perf_model/nuca/bandwidth"))
   , m_queue_model(NULL)
   , m_reads(0)
   , m_writes(0)
   , m_read_misses(0)
   , m_write_misses(0)
{
   m_cache = new Cache("nuca-cache",
      "perf_model/nuca/cache",
      m_core_id,
      parameters.size,
      parameters.associativity,
      m_cache_block_size,
      parameters.replacement_policy,
      CacheBase::PR_L1_CACHE,
      CacheBase::parseAddressHash(parameters.hash_function),
      NULL, /* FaultinjectionManager */
      home_lookup
   );

   if (Sim()->getCfg()->getBool("perf_model/nuca/queue_model/enabled"))
   {
      String queue_model_type = Sim()->getCfg()->getString("perf_model/nuca/queue_model/type");
      m_queue_model = QueueModel::create("nuca-cache-queue", m_core_id, queue_model_type, m_data_array_bandwidth.getRoundedLatency(8 * m_cache_block_size)); // bytes to bits
   }

   registerStatsMetric("nuca-cache", m_core_id, "reads", &m_reads);
   registerStatsMetric("nuca-cache", m_core_id, "writes", &m_writes);
   registerStatsMetric("nuca-cache", m_core_id, "read-misses", &m_read_misses);
   registerStatsMetric("nuca-cache", m_core_id, "write-misses", &m_write_misses);
}

NucaCache::~NucaCache()
{
   delete m_cache;
   if (m_queue_model)
      delete m_queue_model;
}

boost::tuple<SubsecondTime, HitWhere::where_t>
NucaCache::read(IntPtr address, Byte* data_buf, SubsecondTime now, ShmemPerf *perf)
{
   HitWhere::where_t hit_where = HitWhere::MISS;
   perf->updateTime(now);

   PrL1CacheBlockInfo* block_info = (PrL1CacheBlockInfo*)m_cache->peekSingleLine(address);
   SubsecondTime latency = m_tags_access_time.getLatency();
   perf->updateTime(now + latency, ShmemPerf::NUCA_TAGS);

   if (block_info)
   {
      m_cache->accessSingleLine(address, Cache::LOAD, data_buf, m_cache_block_size, now + latency, true);

      latency += accessDataArray(Cache::LOAD, now + latency, perf);
      hit_where = HitWhere::NUCA_CACHE;
   }
   else
   {
      ++m_read_misses;
   }
   ++m_reads;

   return boost::tuple<SubsecondTime, HitWhere::where_t>(latency, hit_where);
}

boost::tuple<SubsecondTime, HitWhere::where_t>
NucaCache::write(IntPtr address, Byte* data_buf, bool& eviction, IntPtr& evict_address, Byte* evict_buf, SubsecondTime now)
{
   HitWhere::where_t hit_where = HitWhere::MISS;

   PrL1CacheBlockInfo* block_info = (PrL1CacheBlockInfo*)m_cache->peekSingleLine(address);
   SubsecondTime latency = m_tags_access_time.getLatency();

   if (block_info)
   {
      block_info->setCState(CacheState::MODIFIED);
      m_cache->accessSingleLine(address, Cache::STORE, data_buf, m_cache_block_size, now + latency, true);

      latency += accessDataArray(Cache::STORE, now + latency, NULL);
      hit_where = HitWhere::NUCA_CACHE;
   }
   else
   {
      PrL1CacheBlockInfo evict_block_info;

      m_cache->insertSingleLine(address, data_buf,
         &eviction, &evict_address, &evict_block_info, evict_buf,
         now + latency);

      if (eviction)
      {
         if (evict_block_info.getCState() != CacheState::MODIFIED)
         {
            // Unless data is dirty, don't have caller write it back
            eviction = false;
         }
      }

      ++m_write_misses;
   }
   ++m_writes;

   return boost::tuple<SubsecondTime, HitWhere::where_t>(latency, hit_where);
}

SubsecondTime
NucaCache::accessDataArray(Cache::access_t access, SubsecondTime t_start, ShmemPerf *perf)
{
   perf->updateTime(t_start);

   // Compute Queue Delay
   SubsecondTime queue_delay;
   if (m_queue_model)
   {
      SubsecondTime processing_time = m_data_array_bandwidth.getRoundedLatency(8 * m_cache_block_size); // bytes to bits

      queue_delay = processing_time + m_queue_model->computeQueueDelay(t_start, processing_time, m_core_id);

      perf->updateTime(t_start + processing_time, ShmemPerf::NUCA_BUS);
      perf->updateTime(t_start + queue_delay, ShmemPerf::NUCA_QUEUE);
   }
   else
   {
      queue_delay = SubsecondTime::Zero();
   }

   perf->updateTime(t_start + queue_delay + m_data_access_time.getLatency(), ShmemPerf::NUCA_DATA);

   return queue_delay + m_data_access_time.getLatency();
}
