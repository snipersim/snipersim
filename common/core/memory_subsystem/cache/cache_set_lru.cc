#include "cache_set.h"
#include "log.h"
#include "stats.h"

CacheSetLRU::CacheSetLRU(
      CacheBase::cache_t cache_type,
      UInt32 associativity, UInt32 blocksize, CacheSetInfoLRU* set_info)
   : CacheSet(cache_type, associativity, blocksize)
   , m_set_info(set_info)
{
   m_lru_bits = new UInt8[m_associativity];
   for (UInt32 i = 0; i < m_associativity; i++)
      m_lru_bits[i] = i;
}

CacheSetLRU::~CacheSetLRU()
{
   delete [] m_lru_bits;
}

UInt32
CacheSetLRU::getReplacementIndex()
{
   // Invalidations may mess up the LRU bits, so find either a free way or the one with the highest LRU bits
   UInt32 index = 0;
   UInt8 max_bits = 0;
   for (UInt32 i = 0; i < m_associativity; i++)
   {
      if (!m_cache_block_info_array[i]->isValid())
      {
         // Mark our newly-inserted line as most-recently used
         moveToMRU(i);
         return i;
      }
      else if (m_lru_bits[i] > max_bits && isValidReplacement(i) )
      {
         index = i;
         max_bits = m_lru_bits[i];
      }
   }

   LOG_ASSERT_ERROR(index < m_associativity, "Error Finding LRU bits");

   // Mark our newly-inserted line as most-recently used
   moveToMRU(index);
   return index;
}

void
CacheSetLRU::updateReplacementIndex(UInt32 accessed_index)
{
   m_set_info->increment(m_lru_bits[accessed_index]);
   moveToMRU(accessed_index);
}

void
CacheSetLRU::moveToMRU(UInt32 accessed_index)
{
   for (UInt32 i = 0; i < m_associativity; i++)
   {
      if (m_lru_bits[i] < m_lru_bits[accessed_index])
         m_lru_bits[i] ++;
   }
   m_lru_bits[accessed_index] = 0;
}

CacheSetInfoLRU::CacheSetInfoLRU(String name, String cfgname, core_id_t core_id, UInt32 associativity)
   : m_associativity(associativity)
{
   m_access = new UInt64[m_associativity];

   for(UInt32 i = 0; i < m_associativity; ++i)
   {
      m_access[i] = 0;
      registerStatsMetric(name, core_id, String("access-mru-")+itostr(i), &m_access[i]);
   }
};

CacheSetInfoLRU::~CacheSetInfoLRU()
{
   delete [] m_access;
}
