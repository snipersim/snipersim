#include "utopia_cache_template.h"
#include "stats.h"
#include "config.hpp"
#include "cache_set.h"
#include <iostream>

namespace ParametricDramDirectoryMSI
{

  UtopiaCache::UtopiaCache(String name, String cfgname, core_id_t core_id, UInt32 block_size, UInt32 size_kb, UInt32 associativity,
                           ComponentLatency _access_latency, ComponentLatency _miss_latency)
    : m_access(0)
    , m_miss(0)
    , m_unique_cache_lines(0)
    , m_core_id(core_id)
    , access_latency(_access_latency)
    , miss_latency(_miss_latency)
    , m_cache(name, cfgname, core_id, k_KILO * size_kb / (associativity * block_size), associativity, block_size, "lru", CacheBase::PR_L1_CACHE)
  {
    std::cout << "[MMU:UTOPIA] Instantiating " << name << " for core " << core_id << " with " << k_KILO * size_kb / (associativity * block_size)
              << " sets, " << associativity << "-way, " << block_size << "-byte lines" << std::endl;
    registerStatsMetric(name, core_id, "access", &m_access);
    registerStatsMetric(name, core_id, "miss", &m_miss);
    registerStatsMetric(name, core_id, "unique_cache_lines", &m_unique_cache_lines);
  }

  UtopiaCache::where_t UtopiaCache::lookup(IntPtr address, SubsecondTime now, bool allocate_on_miss, bool count)
  {
    bool hit = m_cache.accessSingleLine(address, Cache::LOAD, NULL, 0, now, true);

    if (count)
    {
      m_access++;
      IntPtr cache_line_addr = address & ~(IntPtr)63;
      if (m_unique_lines_set.insert(cache_line_addr).second)
        m_unique_cache_lines = m_unique_lines_set.size();
    }

    if (hit)
      return HIT;
    if (count)
      m_miss++;
    if (allocate_on_miss)
      allocate(address, now);
    return MISS;
  }

  void UtopiaCache::allocate(IntPtr address, SubsecondTime now)
  {
    bool eviction;
    IntPtr evict_addr;
    CacheBlockInfo evict_block_info;
    m_cache.insertSingleLine(address, NULL, &eviction, &evict_addr, &evict_block_info, NULL, now);
  }

  bool UtopiaCache::invalidate(IntPtr address)
  {
    return m_cache.invalidateSingleLine(address);
  }

}
