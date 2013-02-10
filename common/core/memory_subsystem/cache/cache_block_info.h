#ifndef __CACHE_BLOCK_INFO_H__
#define __CACHE_BLOCK_INFO_H__

#include "fixed_types.h"
#include "cache_state.h"
#include "cache_base.h"

class CacheBlockInfo
{
   // This can be extended later to include other information
   // for different cache coherence protocols
   private:
      IntPtr m_tag;
      CacheState::cstate_t m_cstate;
      UInt8 m_options;  // large enough to hold a bitfield for all available option_t's

      static const char* option_names[];

   public:
      enum option_t
      {
         PREFETCH,
         WARMUP,
         NUM_OPTIONS
      };

      CacheBlockInfo(IntPtr tag = ~0,
            CacheState::cstate_t cstate = CacheState::INVALID,
            UInt64 options = 0);
      virtual ~CacheBlockInfo();

      static CacheBlockInfo* create(CacheBase::cache_t cache_type);

      virtual void invalidate(void);
      virtual void clone(CacheBlockInfo* cache_block_info);

      bool isValid() const { return (m_tag != ((IntPtr) ~0)); }

      IntPtr getTag() const { return m_tag; }
      CacheState::cstate_t getCState() const { return m_cstate; }

      void setTag(IntPtr tag) { m_tag = tag; }
      void setCState(CacheState::cstate_t cstate) { m_cstate = cstate; }

      bool hasOption(option_t option) { return m_options & (1 << option); }
      void setOption(option_t option) { m_options |= (1 << option); }
      void clearOption(option_t option) { m_options &= ~(UInt64(1) << option); }

      static const char* getOptionName(option_t option);
};

#endif /* __CACHE_BLOCK_INFO_H__ */
