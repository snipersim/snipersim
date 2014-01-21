#ifndef __HIT_WHERE
#define __HIT_WHERE

#include "mem_component.h"

#include <cstddef>
#include <functional>

class HitWhere
{
   public:
      enum where_t
      {
         WHERE_FIRST = 0,
         L1I = MemComponent::L1_ICACHE,
         L1_OWN = MemComponent::L1_DCACHE,
         L2_OWN = MemComponent::L2_CACHE,
         L3_OWN = MemComponent::L3_CACHE,
         L4_OWN = MemComponent::L4_CACHE,
         MISS,
         NUCA_CACHE,
         DRAM_CACHE,
         DRAM,
         DRAM_LOCAL,
         DRAM_REMOTE,
         CACHE_REMOTE,
         SIBLING,
         L1_SIBLING = MemComponent::L1_DCACHE + SIBLING,
         L2_SIBLING = MemComponent::L2_CACHE + SIBLING,
         L3_SIBLING = MemComponent::L3_CACHE + SIBLING,
         L4_SIBLING = MemComponent::L4_CACHE + SIBLING,
         UNKNOWN,
         PREDICATE_FALSE, // CMOV for which the predicate was false, did not actually execute
         PREFETCH_NO_MAPPING,
         NUM_HITWHERES
      };
};

namespace std
{
   template <> struct hash<HitWhere::where_t> {
      size_t operator()(const HitWhere::where_t & type) const {
         return (int)type;
      }
   };
}

const char * HitWhereString(HitWhere::where_t where);
bool HitWhereIsValid(HitWhere::where_t where);

#endif /* __HIT_WHERE */
