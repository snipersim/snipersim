#include "cache_set.h"
#include "log.h"

CacheSetLRU::CacheSetLRU(
      CacheBase::cache_t cache_type,
      UInt32 associativity, UInt32 blocksize) :
   CacheSet(cache_type, associativity, blocksize)
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
         return i;
      }
      else if (m_lru_bits[i] > max_bits)
      {
         index = i;
         max_bits = m_lru_bits[i];
      }
   }

   LOG_ASSERT_ERROR(index < m_associativity, "Error Finding LRU bits");
   return index;
}

void
CacheSetLRU::updateReplacementIndex(UInt32 accessed_index)
{
   for (UInt32 i = 0; i < m_associativity; i++)
   {
      if (m_lru_bits[i] < m_lru_bits[accessed_index])
         m_lru_bits[i] ++;
   }
   m_lru_bits[accessed_index] = 0;
}
