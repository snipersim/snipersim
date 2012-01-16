#include "cache_block_info.h"
#include "pr_l1_cache_block_info.h"
#include "pr_l2_cache_block_info.h"
#include "shared_cache_block_info.h"
#include "log.h"

CacheBlockInfo::CacheBlockInfo(IntPtr tag, CacheState::cstate_t cstate):
   m_tag(tag),
   m_cstate(cstate)
{}

CacheBlockInfo::~CacheBlockInfo()
{}

CacheBlockInfo*
CacheBlockInfo::create(CacheBase::cache_t cache_type)
{
   switch (cache_type)
   {
      case CacheBase::PR_L1_CACHE:
         return new PrL1CacheBlockInfo();

      case CacheBase::PR_L2_CACHE:
         return new PrL2CacheBlockInfo();

      case CacheBase::SHARED_CACHE:
         return new SharedCacheBlockInfo();

      default:
         LOG_PRINT_ERROR("Unrecognized cache type (%u)", cache_type);
         return NULL;
   }
}

void
CacheBlockInfo::invalidate()
{
   m_tag = ~0;
   m_cstate = CacheState::INVALID;
}

void
CacheBlockInfo::clone(CacheBlockInfo* cache_block_info)
{
   m_tag = cache_block_info->getTag();
   m_cstate = cache_block_info->getCState();
}
