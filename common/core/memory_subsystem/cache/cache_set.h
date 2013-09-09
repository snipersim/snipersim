#ifndef CACHE_SET_H
#define CACHE_SET_H

#include "fixed_types.h"
#include "cache_block_info.h"
#include "cache_base.h"
#include "lock.h"
#include "random.h"
#include "log.h"

#include <cstring>

// Per-cache object to store replacement-policy related info (e.g. statistics),
// can collect data from all CacheSet* objects which are per set and implement the actual replacement policy
class CacheSetInfo
{
   public:
      virtual ~CacheSetInfo() {}
};

// Everything related to cache sets
class CacheSet
{
   public:

      static CacheSet* createCacheSet(String cfgname, core_id_t core_id, String replacement_policy, CacheBase::cache_t cache_type, UInt32 associativity, UInt32 blocksize, CacheSetInfo* set_info = NULL);
      static CacheSetInfo* createCacheSetInfo(String name, String cfgname, core_id_t core_id, String replacement_policy, UInt32 associativity);
      static CacheBase::ReplacementPolicy parsePolicyType(String policy);

   protected:
      CacheBlockInfo** m_cache_block_info_array;
      char* m_blocks;
      UInt32 m_associativity;
      UInt32 m_blocksize;
      Lock m_lock;

   public:

      CacheSet(CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize);
      virtual ~CacheSet();

      UInt32 getBlockSize() { return m_blocksize; }
      UInt32 getAssociativity() { return m_associativity; }
      Lock& getLock() { return m_lock; }

      void read_line(UInt32 line_index, UInt32 offset, Byte *out_buff, UInt32 bytes, bool update_replacement);
      void write_line(UInt32 line_index, UInt32 offset, Byte *in_buff, UInt32 bytes, bool update_replacement);
      CacheBlockInfo* find(IntPtr tag, UInt32* line_index = NULL);
      bool invalidate(IntPtr& tag);
      void insert(CacheBlockInfo* cache_block_info, Byte* fill_buff, bool* eviction, CacheBlockInfo* evict_block_info, Byte* evict_buff);

      CacheBlockInfo* peekBlock(UInt32 way) const { return m_cache_block_info_array[way]; }

      char* getDataPtr(UInt32 line_index, UInt32 offset = 0);
      UInt32 getBlockSize(void) const { return m_blocksize; }

      virtual UInt32 getReplacementIndex() = 0;
      virtual void updateReplacementIndex(UInt32) = 0;

      bool isValidReplacement(UInt32 index);
};

class CacheSetRoundRobin : public CacheSet
{
   public:
      CacheSetRoundRobin(CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize);
      ~CacheSetRoundRobin();

      UInt32 getReplacementIndex();
      void updateReplacementIndex(UInt32 accessed_index);

   private:
      UInt32 m_replacement_index;
};

class CacheSetInfoLRU : public CacheSetInfo
{
   public:
      CacheSetInfoLRU(String name, String cfgname, core_id_t core_id, UInt32 associativity);
      virtual ~CacheSetInfoLRU();
      void increment(UInt32 index)
      {
         LOG_ASSERT_ERROR(index < m_associativity, "Index(%d) >= Associativity(%d)", index, m_associativity);
         ++m_access[index];
      }
   private:
      const UInt32 m_associativity;
      UInt64* m_access;
};

class CacheSetLRU : public CacheSet
{
   public:
      CacheSetLRU(CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize, CacheSetInfoLRU* set_info);
      ~CacheSetLRU();

      UInt32 getReplacementIndex();
      void updateReplacementIndex(UInt32 accessed_index);

   private:
      UInt8* m_lru_bits;
      CacheSetInfoLRU* m_set_info;
      void moveToMRU(UInt32 accessed_index);
};

class CacheSetNRU : public CacheSet
{
   public:
      CacheSetNRU(CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize);
      ~CacheSetNRU();

      UInt32 getReplacementIndex();
      void updateReplacementIndex(UInt32 accessed_index);

   private:
      UInt8* m_lru_bits;
      UInt8  m_num_bits_set;
      UInt8  m_replacement_pointer;
};

class CacheSetMRU : public CacheSet
{
   public:
      CacheSetMRU(CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize);
      ~CacheSetMRU();

      UInt32 getReplacementIndex();
      void updateReplacementIndex(UInt32 accessed_index);

   private:
      UInt8* m_lru_bits;
};

class CacheSetNMRU : public CacheSet
{
   public:
      CacheSetNMRU(CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize);
      ~CacheSetNMRU();

      UInt32 getReplacementIndex();
      void updateReplacementIndex(UInt32 accessed_index);

   private:
      UInt8* m_lru_bits;
      UInt8  m_replacement_pointer;
};

class CacheSetPLRU : public CacheSet
{
   public:
      CacheSetPLRU(CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize);
      ~CacheSetPLRU();

      UInt32 getReplacementIndex();
      void updateReplacementIndex(UInt32 accessed_index);

   private:
      UInt8 b[8];
};

class CacheSetSRRIP : public CacheSet
{
   public:
      CacheSetSRRIP(String cfgname, core_id_t core_id,
            CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize);
      ~CacheSetSRRIP();

      UInt32 getReplacementIndex();
      void updateReplacementIndex(UInt32 accessed_index);

   private:
      const UInt8 m_rrip_numbits;
      const UInt8 m_rrip_max;
      const UInt8 m_rrip_insert;
      UInt8* m_rrip_bits;
      UInt8  m_replacement_pointer;
};

class CacheSetRandom : public CacheSet
{
   public:
      CacheSetRandom(CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize);
      ~CacheSetRandom();

      UInt32 getReplacementIndex();
      void updateReplacementIndex(UInt32 accessed_index);

   private:
      Random m_rand;
};

#endif /* CACHE_SET_H */
