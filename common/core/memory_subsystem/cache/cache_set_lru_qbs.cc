#include "cache_set.h"
#include "log.h"
#include "stats.h"

// Implements Query-Based Selection [Jaleel et al., MICRO'10]

CacheSetLRUQBS::CacheSetLRUQBS(
      CacheBase::cache_t cache_type,
      UInt32 associativity, UInt32 blocksize, CacheSetInfoLRUQBS* set_info, UInt8 num_attempts)
   : CacheSetLRU(cache_type, associativity, blocksize, set_info)
   , m_num_attempts(num_attempts)
{
}

UInt32
CacheSetLRUQBS::getReplacementIndex(CacheCntlr *cntlr)
{
   // First try to find an invalid block
   for (UInt32 i = 0; i < m_associativity; i++)
   {
      if (!m_cache_block_info_array[i]->isValid())
      {
         // Mark our newly-inserted line as most-recently used
         moveToMRU(i);
         return i;
      }
   }

   // Make m_num_attemps attempts at evicting the block at LRU position
   for(UInt8 attempt = 0; attempt < m_num_attempts; ++attempt)
   {
      UInt32 index = 0;
      UInt8 max_bits = 0;
      for (UInt32 i = 0; i < m_associativity; i++)
      {
         if (m_lru_bits[i] > max_bits && isValidReplacement(i))
         {
            index = i;
            max_bits = m_lru_bits[i];
         }
      }
      LOG_ASSERT_ERROR(index < m_associativity, "Error Finding LRU bits");

      LOG_ASSERT_ERROR(cntlr != NULL, "CacheCntlr == NULL, QBS can only be used when cntlr is passed in");
      bool in_lower = cntlr->isInLowerLevelCache(m_cache_block_info_array[index]);

      if (in_lower && attempt < m_num_attempts - 1)
      {
         // Block is contained in lower-level cache, and we have more tries remaining.
         // Move this block to MRU and try again
         moveToMRU(index);
         cntlr->incrementQBSLookupCost();
         continue;
      }
      else
      {
         // Mark our newly-inserted line as most-recently used
         moveToMRU(index);
         dynamic_cast<CacheSetInfoLRUQBS*>(m_set_info)->incrementAttempt(attempt);
         return index;
      }
   }

   LOG_PRINT_ERROR("Should not reach here");
}

CacheSetInfoLRUQBS::CacheSetInfoLRUQBS(String name, String cfgname, core_id_t core_id, UInt32 associativity, UInt8 num_attempts)
   : CacheSetInfoLRU(name, cfgname, core_id, associativity)
{
   m_attempts = new UInt64[num_attempts];

   for(UInt32 i = 0; i < num_attempts; ++i)
   {
      m_attempts[i] = 0;
      registerStatsMetric(name, core_id, String("qbs-attempt-")+itostr(i), &m_attempts[i]);
   }
};

CacheSetInfoLRUQBS::~CacheSetInfoLRUQBS()
{
   delete [] m_attempts;
}
