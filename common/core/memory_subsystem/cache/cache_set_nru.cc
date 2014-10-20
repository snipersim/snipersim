#include "cache_set_nru.h"
#include "log.h"

// NRU: Not Recently Used. Some sort of Pseudo LRU policy.

CacheSetNRU::CacheSetNRU(
      CacheBase::cache_t cache_type,
      UInt32 associativity, UInt32 blocksize) :
   CacheSet(cache_type, associativity, blocksize)
{
   m_lru_bits = new UInt8[m_associativity];
   for (UInt32 i = 0; i < m_associativity; i++)
      m_lru_bits[i] = 0;  // initially, lru bits of each set are set to zero, they are not touched yet

   m_num_bits_set = 0;
   m_replacement_pointer = 0;
}

CacheSetNRU::~CacheSetNRU()
{
   delete [] m_lru_bits;
}

UInt32
CacheSetNRU::getReplacementIndex(CacheCntlr *cntlr)
{
   // Invalidations may mess up the LRU bits

   bool have_zero_bit = false;

   for (UInt32 i = 0; i < m_associativity; i++)
   {
      if (!m_cache_block_info_array[i]->isValid())
      {
         // If there is an invalid line(s) in the set, regardless of the LRU bits of other lines, we choose the first invalid line to replace
         // Mark our newly-inserted line as recently used
         updateReplacementIndex(i);
         return i;
      }
      else if (m_lru_bits[i] == 0 && isValidReplacement(i))
      {
         // Check if there are any valid replacement candidates with their LRU bit set to zero
         // If there are none, we'll ignore the LRU bit below and select the first entry with isValidReplacement() as the victim
         have_zero_bit = true;
      }
   }

   for (UInt32 i = 0; i < m_associativity; i++)
   {
      if ((m_lru_bits[m_replacement_pointer] == 0 || have_zero_bit == false)
          && isValidReplacement(m_replacement_pointer))
      {
         // We choose the first non-touched line as the victim (note that we start searching from the replacement pointer position)
         UInt8 index = m_replacement_pointer;
         m_replacement_pointer = (m_replacement_pointer + 1) % m_associativity;

         // Mark our newly-inserted line as recently used
         updateReplacementIndex(index);
         return index;
      }

      m_replacement_pointer = (m_replacement_pointer + 1) % m_associativity;
   }

   LOG_PRINT_ERROR("Error Finding LRU bits");
}

void
CacheSetNRU::updateReplacementIndex(UInt32 accessed_index)
{
   m_lru_bits[accessed_index] = 1;
   m_num_bits_set++;

   // If all lru bits are set to 1 in the set, we make all of them 0

   if (m_num_bits_set == m_associativity)
   {
      m_num_bits_set = 0;

      for (UInt32 i = 0; i < m_associativity; i++)
      {
         m_lru_bits[i] = 0;
      }
   }
}
