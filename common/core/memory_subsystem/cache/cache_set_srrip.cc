#include "cache_set.h"
#include "simulator.h"
#include "config.hpp"
#include "log.h"

// S-RRIP: Static Re-reference Interval Prediction policy

CacheSetSRRIP::CacheSetSRRIP(
      String cfgname, core_id_t core_id,
      CacheBase::cache_t cache_type,
      UInt32 associativity, UInt32 blocksize)
   : CacheSet(cache_type, associativity, blocksize)
   , m_rrip_numbits(Sim()->getCfg()->getIntArray(cfgname + "/srrip/bits", core_id))
   , m_rrip_max((1 << m_rrip_numbits) - 1)
   , m_rrip_insert(m_rrip_max - 1)
   , m_replacement_pointer(0)
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
CacheSetSRRIP::getReplacementIndex()
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

   for(UInt32 j = 0; j <= m_rrip_max; ++j)
   {
      for (UInt32 i = 0; i < m_associativity; i++)
      {
         if (m_rrip_bits[m_replacement_pointer] == m_rrip_max)
         {
            // We choose the first non-touched line as the victim (note that we start searching from the replacement pointer position)
            UInt8 index = m_replacement_pointer;
            m_replacement_pointer = (m_replacement_pointer + 1) % m_associativity;
            // Prepare way for a new line: set prediction to 'long'
            m_rrip_bits[index] = m_rrip_insert;

            LOG_ASSERT_ERROR(isValidReplacement(index), "SRRIP selected an invalid replacement candidate" );
            return index;
         }

         m_replacement_pointer = (m_replacement_pointer + 1) % m_associativity;
      }

      // Increment all RRIP counters until one hits RRIP_MAX
      for (UInt32 i = 0; i < m_associativity; i++)
      {
         m_rrip_bits[i]++;
      }
   }

   LOG_PRINT_ERROR("Error finding replacement index");
}

void
CacheSetSRRIP::updateReplacementIndex(UInt32 accessed_index)
{
   if (m_rrip_bits[accessed_index] > 0)
      m_rrip_bits[accessed_index]--;
}
