#ifndef __CACHE_ATD_H
#define __CACHE_ATD_H

#include "fixed_types.h"
#include "cache_base.h"
#include "core.h"

class CacheSet;

class ATD
{
   private:
      CacheBase m_cache_base;
      std::unordered_map<UInt32, CacheSet*> m_sets;

      UInt64 loads, stores;
      UInt64 load_misses, store_misses;
      UInt64 loads_constructive, stores_constructive;
      UInt64 loads_destructive, stores_destructive;

      bool isSampledSet(UInt32 set_index);

   public:
      ATD(String name, String configName, core_id_t core_id, UInt32 num_sets, UInt32 associativity,
          UInt32 cache_block_size, String replacement_policy, CacheBase::hash_t hash_function);
      ~ATD() {}

      void access(Core::mem_op_t mem_op_type, bool hit, IntPtr address);
};

#endif // __CACHE_ATD_H
