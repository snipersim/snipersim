#ifndef CACHE_SET_H
#define CACHE_SET_H

#include <string.h>

#include "fixed_types.h"
#include "cache_block_info.h"
#include "cache_base.h"
#include "lock.h"
#include "random.h"

// Everything related to cache sets
class CacheSet
{
   public:

      static CacheSet* createCacheSet(String cfgname, core_id_t core_id, String replacement_policy, CacheBase::cache_t cache_type, UInt32 associativity, UInt32 blocksize);
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

      void read_line(UInt32 line_index, UInt32 offset, Byte *out_buff, UInt32 bytes);
      void write_line(UInt32 line_index, UInt32 offset, Byte *in_buff, UInt32 bytes);
      CacheBlockInfo* find(IntPtr tag, UInt32* line_index = NULL);
      bool invalidate(IntPtr& tag);
      void insert(CacheBlockInfo* cache_block_info, Byte* fill_buff, bool* eviction, CacheBlockInfo* evict_block_info, Byte* evict_buff);

      char* getDataPtr(UInt32 line_index, UInt32 offset = 0);
      UInt32 getBlockSize(void) const { return m_blocksize; }

      virtual UInt32 getReplacementIndex() = 0;
      virtual void updateReplacementIndex(UInt32) = 0;
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

class CacheSetLRU : public CacheSet
{
   public:
      CacheSetLRU(CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize);
      ~CacheSetLRU();

      UInt32 getReplacementIndex();
      void updateReplacementIndex(UInt32 accessed_index);

   private:
      UInt8* m_lru_bits;
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
