#include "shared_cache_block_info.h"
#include "log.h"

#ifdef ENABLE_TRACK_SHARING_PREVCACHES

PrevCacheIndex
SharedCacheBlockInfo::getCachedLoc()
{
   LOG_ASSERT_ERROR(m_cached_locs.count() == 1, "m_cached_locs.count() == %u", m_cached_locs.count());

   for(PrevCacheIndex idx = 0; idx < m_cached_locs.size(); ++idx)
      if (m_cached_locs.test(idx))
         return idx;
   assert(false);
}

void
SharedCacheBlockInfo::setCachedLoc(PrevCacheIndex idx)
{
   LOG_ASSERT_ERROR(m_cached_locs.test(idx) == false, "location %u already in set", idx);

   m_cached_locs.set(idx);
}

void
SharedCacheBlockInfo::clearCachedLoc(PrevCacheIndex idx)
{
   LOG_ASSERT_ERROR(m_cached_locs.test(idx) == true, "location %u not set", idx);

   m_cached_locs.reset(idx);
}

#endif

void
SharedCacheBlockInfo::invalidate()
{
   #ifdef ENABLE_TRACK_SHARING_PREVCACHES
   for(PrevCacheIndex idx = 0; idx < m_cached_locs.size(); ++idx)
      m_cached_locs.reset(idx);
   #endif
   CacheBlockInfo::invalidate();
}

void
SharedCacheBlockInfo::clone(CacheBlockInfo* cache_block_info)
{
   #ifdef ENABLE_TRACK_SHARING_PREVCACHES
   m_cached_locs = ((SharedCacheBlockInfo*) cache_block_info)->getCachedLocs();
   #endif
   CacheBlockInfo::clone(cache_block_info);
}
