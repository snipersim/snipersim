#include "tlb.h"
#include "stats.h"

namespace ParametricDramDirectoryMSI
{

TLB::TLB(String name, String cfgname, core_id_t core_id, UInt32 size, UInt32 associativity)
   : m_size(size)
   , m_associativity(associativity)
   , m_cache(name + "_cache", cfgname, core_id, size, associativity, SIM_PAGE_SIZE, "lru", CacheBase::PR_L1_CACHE)
   , m_access(0)
   , m_miss(0)
{
   registerStatsMetric(name, core_id, "access", &m_access);
   registerStatsMetric(name, core_id, "miss", &m_miss);
}

bool
TLB::lookup(IntPtr address, SubsecondTime now)
{
   m_access++;

   if (m_cache.accessSingleLine(address, Cache::LOAD, NULL, 0, now))
      return true;

   m_miss++;

   bool eviction;
   IntPtr evict_addr;
   CacheBlockInfo evict_block_info;
   m_cache.insertSingleLine(address, NULL, &eviction, &evict_addr, &evict_block_info, NULL, now);

   return false;
}

}
