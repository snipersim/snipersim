#include "cache_base.h"
#include "utils.h"
#include "log.h"
#include "rng.h"
#include "address_home_lookup.h"

CacheBase::CacheBase(
   String name, UInt32 num_sets, UInt32 associativity, UInt32 cache_block_size,
   CacheBase::hash_t hash, AddressHomeLookup *ahl)
:
   m_name(name),
   m_cache_size(UInt64(num_sets) * associativity * cache_block_size),
   m_associativity(associativity),
   m_blocksize(cache_block_size),
   m_hash(hash),
   m_num_sets(num_sets),
   m_ahl(ahl)
{
   m_log_blocksize = floorLog2(m_blocksize);

   LOG_ASSERT_ERROR((m_num_sets == (1UL << floorLog2(m_num_sets))) || (hash != CacheBase::HASH_MASK),
      "Caches of non-power of 2 size need funky hash function");
}

CacheBase::~CacheBase()
{}

// utilities
CacheBase::hash_t
CacheBase::parseAddressHash(String hash_name)
{
   if (hash_name == "mask")
      return CacheBase::HASH_MASK;
   else if (hash_name == "mod")
      return CacheBase::HASH_MOD;
   else if (hash_name == "rng1_mod")
      return CacheBase::HASH_RNG1_MOD;
   else if (hash_name == "rng2_mod")
      return CacheBase::HASH_RNG2_MOD;
   else
      LOG_PRINT_ERROR("Invalid address hash function %s", hash_name.c_str());
}

void
CacheBase::splitAddress(const IntPtr addr, IntPtr& tag, UInt32& set_index) const
{
   tag = addr >> m_log_blocksize;

   IntPtr linearAddress = m_ahl ? m_ahl->getLinearAddress(addr) : addr;
   IntPtr block_num = linearAddress >> m_log_blocksize;

   switch(m_hash)
   {
      case CacheBase::HASH_MASK:
         set_index = block_num & (m_num_sets-1);
         break;
      case CacheBase::HASH_MOD:
         set_index = block_num % m_num_sets;
         break;
      case CacheBase::HASH_RNG1_MOD:
      {
         UInt64 state = rng_seed(block_num);
         set_index = rng_next(state) % m_num_sets;
         break;
      }
      case CacheBase::HASH_RNG2_MOD:
      {
         UInt64 state = rng_seed(block_num);
         rng_next(state);
         set_index = rng_next(state) % m_num_sets;
         break;
      }
      default:
         LOG_PRINT_ERROR("Invalid hash function %d", m_hash);
   }
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
