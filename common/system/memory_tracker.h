#ifndef __MEMORY_TRACKER_H
#define __MEMORY_TRACKER_H

#include "fixed_types.h"
#include "lock.h"
#include "routine_tracer.h"
#include "cache_efficiency_tracker.h"

#include <vector>
#include <map>
#include <unordered_map>

class MemoryTracker
{
   public:
      MemoryTracker();
      ~MemoryTracker();

      void logMalloc(thread_id_t thread_id, UInt64 eip, UInt64 address, UInt64 size);
      void logFree(thread_id_t thread_id, UInt64 eip, UInt64 address);

   private:
      struct AllocationSite
      {
         AllocationSite() : total_size(0), hit_where(HitWhere::NUM_HITWHERES, 0) {}
         UInt64 total_size;
         std::vector<UInt64> hit_where;
         std::unordered_map<AllocationSite*, UInt64> evicted_by;
      };
      typedef std::unordered_map<CallStack, AllocationSite*> AllocationSites;

      struct Allocation
      {
         Allocation() : size(0), site(NULL) {}
         Allocation(UInt64 _size, AllocationSite* _site) : size(_size), site(_site) {}
         UInt64 size;
         AllocationSite *site;
      };
      typedef std::map<UInt64, Allocation> Allocations;

      Lock m_lock;
      Allocations m_allocations;
      AllocationSites m_allocation_sites;

      UInt64 ce_get_owner(core_id_t core_id, UInt64 address);
      void ce_notify_access(UInt64 owner, HitWhere::where_t hit_where);
      void ce_notify_evict(bool on_roi_end, UInt64 owner, UInt64 evictor, CacheBlockInfo::BitsUsedType bits_used, UInt32 bits_total);

      static UInt64 __ce_get_owner(UInt64 user, core_id_t core_id, UInt64 address)
      { return ((MemoryTracker*)user)->ce_get_owner(core_id, address); }
      static void __ce_notify_access(UInt64 user, UInt64 owner, HitWhere::where_t hit_where)
      { ((MemoryTracker*)user)->ce_notify_access(owner, hit_where); }
      static void __ce_notify_evict(UInt64 user, bool on_roi_end, UInt64 owner, UInt64 evictor, CacheBlockInfo::BitsUsedType bits_used, UInt32 bits_total)
      { ((MemoryTracker*)user)->ce_notify_evict(on_roi_end, owner, evictor, bits_used, bits_total); }
};

#endif // __MEMORY_TRACKER_H
