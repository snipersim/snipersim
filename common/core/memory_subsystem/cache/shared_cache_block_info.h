#pragma once

#include "cache_state.h"
#include "cache_block_info.h"
#include "mem_component.h"

// Define to enable tracking of which previous-level caches share each cache line
// Currently this is only used for asserts (makeing sure no non-sharers send use evictions)
// but it takes up space, especially when sharing caches with a lot of cores
//#define ENABLE_TRACK_SHARING_PREVCACHES

#ifdef ENABLE_TRACK_SHARING_PREVCACHES
#include <bitset>

const UInt32 MAX_NUM_PREVCACHES = 8;
typedef std::bitset<MAX_NUM_PREVCACHES> CacheSharersType;
typedef UInt8 PrevCacheIndex; // Should hold an integer up to MAX_NUM_PREVCACHES
#endif

class SharedCacheBlockInfo : public CacheBlockInfo
{
   private:
      #ifdef ENABLE_TRACK_SHARING_PREVCACHES
      CacheSharersType m_cached_locs;
      #endif

   public:
      SharedCacheBlockInfo(IntPtr tag = ~0,
            CacheState::cstate_t cstate = CacheState::INVALID)
         : CacheBlockInfo(tag, cstate)
         #ifdef ENABLE_TRACK_SHARING_PREVCACHES
         , m_cached_locs()
         #endif
      {}

      ~SharedCacheBlockInfo() {}

      #ifdef ENABLE_TRACK_SHARING_PREVCACHES
      PrevCacheIndex getCachedLoc();
      bool hasCachedLoc() { return m_cached_locs.any(); }
      void setCachedLoc(PrevCacheIndex idx);
      void clearCachedLoc(PrevCacheIndex idx);

      CacheSharersType getCachedLocs() { return m_cached_locs; }
      #endif

      void invalidate();
      void clone(CacheBlockInfo* cache_block_info);
};
