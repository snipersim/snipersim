#pragma once

#include "cache_state.h"
#include "cache_block_info.h"
#include "mem_component.h"

#include <bitset>

const UInt32 MAX_NUM_PREVCACHES = 8;
typedef std::bitset<MAX_NUM_PREVCACHES> CacheSharersType;
typedef UInt8 PrevCacheIndex; // Should hold an integer up to MAX_NUM_PREVCACHES

class SharedCacheBlockInfo : public CacheBlockInfo
{
   private:
      CacheSharersType m_cached_locs;

   public:
      SharedCacheBlockInfo(IntPtr tag = ~0,
            CacheState::cstate_t cstate = CacheState::INVALID):
         CacheBlockInfo(tag, cstate),
         m_cached_locs()
      {}

      ~SharedCacheBlockInfo() {}

      PrevCacheIndex getCachedLoc();
      bool hasCachedLoc() { return m_cached_locs.any(); }
      void setCachedLoc(PrevCacheIndex idx);
      void clearCachedLoc(PrevCacheIndex idx);

      CacheSharersType getCachedLocs() { return m_cached_locs; }

      void invalidate();
      void clone(CacheBlockInfo* cache_block_info);
};
