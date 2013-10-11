#include "cache_set_srrip.h"
#include "simulator.h"
#include "config.hpp"
#include "log.h"

// S-RRIP: Static Re-reference Interval Prediction policy

CacheSetSRRIP::CacheSetSRRIP(
      String cfgname, core_id_t core_id,
      CacheBase::cache_t cache_type,
      UInt32 associativity, UInt32 blocksize, CacheSetInfoLRU* set_info, UInt8 num_attempts)
   : CacheSet(cache_type, associativity, blocksize)
   , m_rrip_numbits(Sim()->getCfg()->getIntArray(cfgname + "/srrip/bits", core_id))
   , m_rrip_max((1 << m_rrip_numbits) - 1)
   , m_rrip_insert(m_rrip_max - 1)
   , m_num_attempts(num_attempts)
   , m_replacement_pointer(0)
   , m_set_info(set_info)
{
   m_rrip_bits = new UInt8[m_associativity];
   for (UInt32 i = 0; i < m_associativity; i++)
      m_rrip_bits[i] = m_rrip_insert;
}

CacheSetSRRIP::~CacheSetSRRIP()
{
   delete [] m_rrip_bits;
}

UInt32
CacheSetSRRIP::getReplacementIndex(CacheCntlr *cntlr)
{
   for (UInt32 i = 0; i < m_associativity; i++)
   {
      if (!m_cache_block_info_array[i]->isValid())
      {
         // If there is an invalid line(s) in the set, regardless of the LRU bits of other lines, we choose the first invalid line to replace
         // Prepare way for a new line: set prediction to 'long'
         m_rrip_bits[i] = m_rrip_insert;
         return i;
      }
   }

   UInt8 attempt = 0;

   for(UInt32 j = 0; j <= m_rrip_max; ++j)
   {
      for (UInt32 i = 0; i < m_associativity; i++)
      {
         if (m_rrip_bits[m_replacement_pointer] >= m_rrip_max)
         {
            // We choose the first non-touched line as the victim (note that we start searching from the replacement pointer position)
            UInt8 index = m_replacement_pointer;

            bool qbs_reject = false;
            if (attempt < m_num_attempts - 1)
            {
               LOG_ASSERT_ERROR(cntlr != NULL, "CacheCntlr == NULL, QBS can only be used when cntlr is passed in");
               qbs_reject = cntlr->isInLowerLevelCache(m_cache_block_info_array[index]);
            }

            if (qbs_reject)
            {
               // Block is contained in lower-level cache, and we have more tries remaining.
               // Move this block to MRU and try again
               m_rrip_bits[index] = 0;
               cntlr->incrementQBSLookupCost();
               ++attempt;
               continue;
            }

            m_replacement_pointer = (m_replacement_pointer + 1) % m_associativity;
            // Prepare way for a new line: set prediction to 'long'
            m_rrip_bits[index] = m_rrip_insert;

            m_set_info->incrementAttempt(attempt);

            LOG_ASSERT_ERROR(isValidReplacement(index), "SRRIP selected an invalid replacement candidate" );
            return index;
         }

         m_replacement_pointer = (m_replacement_pointer + 1) % m_associativity;
      }

      // Increment all RRIP counters until one hits RRIP_MAX
      for (UInt32 i = 0; i < m_associativity; i++)
      {
         if (m_rrip_bits[i] < m_rrip_max)
         {
            m_rrip_bits[i]++;
         }
      }
   }

   LOG_PRINT_ERROR("Error finding replacement index");
}

void
CacheSetSRRIP::updateReplacementIndex(UInt32 accessed_index)
{
   m_set_info->increment(m_rrip_bits[accessed_index]);

   if (m_rrip_bits[accessed_index] > 0)
      m_rrip_bits[accessed_index]--;
}
