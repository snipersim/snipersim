#ifndef __CACHE_BASE_H__
#define __CACHE_BASE_H__

#include "fixed_types.h"

class AddressHomeLookup;

#define k_KILO 1024
#define k_MEGA (k_KILO*k_KILO)
#define k_GIGA (k_KILO*k_MEGA)

// Generic cache base class;
// no allocate specialization, no cache set specialization
class CacheBase
{
   public:
      // types, constants
      enum access_t
      {
         INVALID_ACCESS_TYPE,
         MIN_ACCESS_TYPE,
         LOAD = MIN_ACCESS_TYPE,
         STORE,
         MAX_ACCESS_TYPE = STORE,
         NUM_ACCESS_TYPES = MAX_ACCESS_TYPE - MIN_ACCESS_TYPE + 1
      };

      enum cache_t
      {
         INVALID_CACHE_TYPE,
         MIN_CACHE_TYPE,
         PR_L1_CACHE = MIN_CACHE_TYPE,
         PR_L2_CACHE,
         SHARED_CACHE,
         MAX_CACHE_TYPE = SHARED_CACHE,
         NUM_CACHE_TYPES = MAX_CACHE_TYPE - MIN_CACHE_TYPE + 1
      };

      enum hash_t
      {
         INVALID_HASH_TYPE,
         HASH_MASK,
         HASH_MOD,
         HASH_RNG1_MOD,
         HASH_RNG2_MOD,
      };

      enum ReplacementPolicy
      {
         ROUND_ROBIN = 0,
         LRU,
         NRU,
         MRU,
         NMRU,
         PLRU,
         SRRIP,
         RANDOM,
         NUM_REPLACEMENT_POLICIES
      };

   protected:
      // input params
      String m_name;
      UInt64 m_cache_size;
      UInt32 m_associativity;
      UInt32 m_blocksize;
      CacheBase::hash_t m_hash;
      UInt32 m_num_sets;
      AddressHomeLookup *m_ahl;

      // computed params
      UInt32 m_log_blocksize;

   public:
      // constructors/destructors
      CacheBase(String name, UInt32 cache_size, UInt32 associativity, UInt32 cache_block_size, CacheBase::hash_t hash, AddressHomeLookup *ahl = NULL);
      virtual ~CacheBase();

      // utilities
      void splitAddress(const IntPtr addr, IntPtr& tag, UInt32& set_index) const;
      void splitAddress(const IntPtr addr, IntPtr& tag, UInt32& set_index, UInt32& block_offset) const;
      IntPtr tagToAddress(const IntPtr tag);
      String getName(void) { return m_name; }

      UInt32 getNumSets() const { return m_num_sets; }
      UInt32 getAssociativity() const { return m_associativity; }

      static hash_t parseAddressHash(String hash_name);
};

#endif /* __CACHE_BASE_H__ */
