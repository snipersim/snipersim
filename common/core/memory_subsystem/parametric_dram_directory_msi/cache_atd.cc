#include "cache_atd.h"
#include "cache_set.h"
#include "pr_l1_cache_block_info.h"
#include "stats.h"
#include "config.hpp"
#include "rng.h"

ATD::ATD(String name, String configName, core_id_t core_id, UInt32 num_sets, UInt32 associativity,
         UInt32 cache_block_size, String replacement_policy, CacheBase::hash_t hash_function)
   : m_cache_base(name, num_sets, associativity, cache_block_size, hash_function)
   , m_sets()
   , loads(0)
   , stores(0)
   , load_misses(0)
   , store_misses(0)
   , loads_constructive(0)
   , stores_constructive(0)
   , loads_destructive(0)
   , stores_destructive(0)
{
   m_set_info = CacheSet::createCacheSetInfo(name, configName, core_id, replacement_policy, associativity);

   registerStatsMetric(name, core_id, "loads", &loads);
   registerStatsMetric(name, core_id, "stores", &stores);
   registerStatsMetric(name, core_id, "load-misses", &load_misses);
   registerStatsMetric(name, core_id, "store-misses", &store_misses);
   registerStatsMetric(name, core_id, "loads-constructive", &loads_constructive);
   registerStatsMetric(name, core_id, "loads-destructive", &loads_destructive);
   registerStatsMetric(name, core_id, "stores-constructive", &stores_constructive);
   registerStatsMetric(name, core_id, "stores-destructive", &stores_destructive);

   String sampling = Sim()->getCfg()->getStringArray(configName + "/atd/sampling", core_id);
   if (sampling == "full")
   {
      for(UInt64 set_index = 0; set_index < num_sets; ++set_index)
      {
         m_sets[set_index] = CacheSet::createCacheSet(name, core_id, replacement_policy, CacheBase::PR_L1_CACHE, associativity, 0, m_set_info);
      }
   }
   else if (sampling == "2^n+1")
   {
      // Sample sets at indexes 2^N+1
      for(UInt64 set_index = 1; set_index < num_sets - 1; set_index <<= 1)
      {
         m_sets[set_index+1] = CacheSet::createCacheSet(name, core_id, replacement_policy, CacheBase::PR_L1_CACHE, associativity, 0, m_set_info);
      }
   }
   else if (sampling == "random")
   {
      // Sample sets at random positions
      UInt64 state = rng_seed(Sim()->getCfg()->getIntArray(configName + "/atd/sampling/random/seed", core_id));
      UInt64 num_atds = Sim()->getCfg()->getIntArray(configName + "/atd/sampling/random/count", core_id);
      UInt64 num_attempts = 0;

      LOG_ASSERT_ERROR(num_atds <= num_sets, "Cannot sample more sets (%d) than the total number of sets (%d)", num_atds, num_sets);

      while(num_atds)
      {
         UInt64 set_index = rng_next(state) % num_sets;
         if (m_sets.count(set_index) == 0)
         {
            m_sets[set_index] = CacheSet::createCacheSet(name, core_id, replacement_policy, CacheBase::PR_L1_CACHE, associativity, 0, m_set_info);
            --num_atds;
         }
         LOG_ASSERT_ERROR(++num_attempts < 10 * num_sets, "Cound not find unique ATD sets even after many attempts");
      }
   }
   else
   {
      LOG_PRINT_ERROR("Invalid ATD sampling method %s", sampling.c_str());
   }
}

ATD::~ATD()
{
   if (m_set_info)
      delete m_set_info;
}

bool ATD::isSampledSet(UInt32 set_index)
{
   return m_sets.count(set_index);
}

void ATD::access(Core::mem_op_t mem_op_type, bool cache_hit, IntPtr address)
{
   IntPtr tag; UInt32 set_index;
   m_cache_base.splitAddress(address, tag, set_index);

   if (isSampledSet(set_index))
   {
      UInt32 line_index = -1;
      bool atd_hit = m_sets[set_index]->find(tag, &line_index);

      if (atd_hit)
      {
         m_sets[set_index]->updateReplacementIndex(line_index);
      }
      else
      {
         PrL1CacheBlockInfo* cache_block_info = new PrL1CacheBlockInfo(tag, CacheState::MODIFIED);
         bool eviction; PrL1CacheBlockInfo evict_block_info;
         m_sets[set_index]->insert(cache_block_info, NULL, &eviction, &evict_block_info, NULL);
      }

      if (mem_op_type == Core::WRITE)
      {
         ++stores;
         if (!atd_hit)
            ++store_misses;
      }
      else
      {
         ++loads;
         if (!atd_hit)
            ++load_misses;
      }

      if (cache_hit && !atd_hit)
      {
         if (mem_op_type == Core::WRITE)
            ++stores_constructive;
         else
            ++loads_constructive;
      }
      else if (!cache_hit && atd_hit)
      {
         if (mem_op_type == Core::WRITE)
            ++stores_destructive;
         else
            ++loads_destructive;
      }
   }
}
