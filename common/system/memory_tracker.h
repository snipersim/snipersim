#ifndef __MEMORY_TRACKER_H
#define __MEMORY_TRACKER_H

#include "fixed_types.h"
#include "lock.h"
#include "routine_tracer.h"

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
      struct Allocation
      {
         Allocation() : size(0), call_site_id(0) {}
         Allocation(UInt64 _size, UInt64 _site) : size(_size), call_site_id(_site) {}
         UInt64 size;
         UInt64 call_site_id;
      };
      typedef std::map<UInt64, Allocation> Allocations;

      struct AllocationSite
      {
         AllocationSite() : site_id(0) {}
         AllocationSite(UInt64 _id) : site_id(_id), total_size(0) {}
         UInt64 site_id;
         UInt64 total_size;
      };
      typedef std::unordered_map<CallStack, AllocationSite> AllocationSites;

      Lock m_lock;
      Allocations m_allocations;
      AllocationSites m_allocation_sites;
};

#endif // __MEMORY_TRACKER_H
