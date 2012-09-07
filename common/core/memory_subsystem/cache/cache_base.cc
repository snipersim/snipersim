#include "cache_base.h"
#include "utils.h"

CacheBase::CacheBase(String name, UInt32 cache_size, UInt32 associativity, UInt32 cache_block_size):
   m_name(name),
   m_cache_size(k_KILO * UInt64(cache_size)),
   m_associativity(associativity),
   m_blocksize(cache_block_size)
{
   m_num_sets = m_cache_size / (m_associativity * m_blocksize);
   m_log_blocksize = floorLog2(m_blocksize);
}

CacheBase::~CacheBase()
{}

// utilities
void
CacheBase::splitAddress(const IntPtr addr, IntPtr& tag, UInt32& set_index) const
{
   tag = addr >> m_log_blocksize;
   set_index = tag & (m_num_sets-1);
}

void
CacheBase::splitAddress(const IntPtr addr, IntPtr& tag, UInt32& set_index,
                  UInt32& block_offset) const
{
   block_offset = addr & (m_blocksize-1);
   splitAddress(addr, tag, set_index);
}

IntPtr
CacheBase::tagToAddress(const IntPtr tag)
{
   return tag << m_log_blocksize;
}
