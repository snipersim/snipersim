#include "pr_l2_cache_block_info.h"
#include "log.h"

MemComponent::component_t 
PrL2CacheBlockInfo::getCachedLoc()
{
   LOG_ASSERT_ERROR(m_cached_loc_bitvec != ((1 << MemComponent::L1_ICACHE) | (1 << MemComponent::L1_DCACHE)),
         "m_cached_loc_bitvec(%u)", m_cached_loc_bitvec);
   switch(m_cached_loc_bitvec)
   {
      case ((UInt32) 1) << MemComponent::L1_ICACHE:
         return MemComponent::L1_ICACHE;
      
      case ((UInt32) 1) << MemComponent::L1_DCACHE:
         return MemComponent::L1_DCACHE;

      case ((UInt32) 0):
         return MemComponent::INVALID_MEM_COMPONENT;

      default:
         LOG_PRINT_ERROR("Error: m_cached_loc_bitvec(%u)", m_cached_loc_bitvec);
         return MemComponent::INVALID_MEM_COMPONENT;
   }
}

void 
PrL2CacheBlockInfo::setCachedLoc(MemComponent::component_t cached_loc)
{
   LOG_ASSERT_ERROR(cached_loc != MemComponent::INVALID_MEM_COMPONENT,
         "m_cached_loc_bitvec(%u), cached_loc(%u)",
         m_cached_loc_bitvec, cached_loc);
   LOG_ASSERT_ERROR(!(m_cached_loc_bitvec & (1 << cached_loc)),
         "m_cached_loc_bitvec(%u), cached_loc(%u)",
         m_cached_loc_bitvec, cached_loc);
   m_cached_loc_bitvec |= (((UInt32) 1) << cached_loc);
}

void
PrL2CacheBlockInfo::clearCachedLoc(MemComponent::component_t cached_loc)
{
   LOG_ASSERT_ERROR(cached_loc != MemComponent::INVALID_MEM_COMPONENT,
         "m_cached_loc_bitvec(%u), cached_loc(%u)",
         m_cached_loc_bitvec, cached_loc);
   LOG_ASSERT_ERROR(m_cached_loc_bitvec & (1 << cached_loc),
         "m_cached_loc_bitvec(%u), cached_loc(%u)", 
         m_cached_loc_bitvec, cached_loc);
   m_cached_loc_bitvec &= (~(((UInt32) 1) << cached_loc));
}

void 
PrL2CacheBlockInfo::invalidate()
{
   m_cached_loc_bitvec = 0;
   CacheBlockInfo::invalidate();
}

void 
PrL2CacheBlockInfo::clone(CacheBlockInfo* cache_block_info)
{
   m_cached_loc_bitvec = ((PrL2CacheBlockInfo*) cache_block_info)->getCachedLocBitVec();
   CacheBlockInfo::clone(cache_block_info);
}
