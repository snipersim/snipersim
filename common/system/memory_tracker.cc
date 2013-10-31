#include "memory_tracker.h"
#include "simulator.h"
#include "thread_manager.h"
#include "thread.h"

MemoryTracker::MemoryTracker()
{
   Sim()->getConfig()->setCacheEfficiencyCallbacks(__ce_get_owner, __ce_notify_access, __ce_notify_evict, (UInt64)this);
}

MemoryTracker::~MemoryTracker()
{
   for(auto it = m_allocation_sites.begin(); it != m_allocation_sites.end(); ++it)
   {
      const CallStack &stack = it->first;
      const AllocationSite *site = it->second;
      printf("Allocation site %p:\n", site);
      printf("\tCall stack:");
      for(auto jt = stack.begin(); jt != stack.end(); ++jt)
      {
         const RoutineTracer::Routine *rtn = Sim()->getRoutineTracer()->getRoutineInfo(*jt);
         if (rtn)
            printf("\t\t[%lx] %s\n", *jt, rtn->m_name);
         else
            printf("\t\t[%lx] ???\n", *jt);
      }
      printf("\tTotal allocated: %ld bytes\n", site->total_size);
      printf("\tHit-Where:\n");
      for(int h = HitWhere::WHERE_FIRST ; h < HitWhere::NUM_HITWHERES ; h++)
      {
         if (HitWhereIsValid((HitWhere::where_t)h) && site->hit_where[h] > 0)
         {
            printf("\t\t%-20s: %ld\n", HitWhereString((HitWhere::where_t)h), site->hit_where[h]);
         }
      }
      printf("\tEvicted by:\n");
      for(auto it = site->evicted_by.begin(); it != site->evicted_by.end(); ++it)
         printf("\t\t%20p: %ld\n", it->first, it->second);
   }
}

void MemoryTracker::logMalloc(thread_id_t thread_id, UInt64 eip, UInt64 address, UInt64 size)
{
   ScopedLock sl(m_lock);

   const CallStack stack = Sim()->getThreadManager()->getThreadFromID(thread_id)->getRoutineTracer()->getCallStack();

   AllocationSite *site = NULL;
   AllocationSites::iterator it = m_allocation_sites.find(stack);
   if (it != m_allocation_sites.end())
      site = it->second;
   else
   {
      site = new AllocationSite();
      m_allocation_sites[stack] = site;
   }

   //printf("memtracker: site %p(%lx) malloc %lx + %lx\n", site, eip, address, size);

   m_allocation_sites[stack]->total_size += size;
   // Store the first address of the first cache line that no longer belongs to the allocation
   UInt64 lower = address & ~63, upper = (address + size + 63) & ~63;
   m_allocations[upper] = Allocation(upper - lower, site);
}

void MemoryTracker::logFree(thread_id_t thread_id, UInt64 eip, UInt64 address)
{
   ScopedLock sl(m_lock);

   //printf("memtracker: free %lx\n", address);

   auto it = m_allocations.find(address);
   if (it != m_allocations.end())
      m_allocations.erase(it);
}

UInt64 MemoryTracker::ce_get_owner(core_id_t core_id, UInt64 address)
{
   ScopedLock sl(m_lock);

   // upper_bound returns the first entry greater than address
   // Because the key in m_allocations is the first cache line that no longer falls into the range,
   // we will find the correct alloction *if* address falls within it
   auto upper = m_allocations.upper_bound(address);
   if (upper != m_allocations.end() && address >= upper->first - upper->second.size)
      return (UInt64)upper->second.site;
   else
      return 0;
}

void MemoryTracker::ce_notify_access(UInt64 owner, HitWhere::where_t hit_where)
{
   if (owner)
   {
      AllocationSite *site = (AllocationSite*)owner;
      site->hit_where[hit_where]++;
   }
}

void MemoryTracker::ce_notify_evict(bool on_roi_end, UInt64 owner, UInt64 evictor, CacheBlockInfo::BitsUsedType bits_used, UInt32 bits_total)
{
   if (!on_roi_end && owner)
   {
      AllocationSite *site = (AllocationSite*)owner;
      AllocationSite *evictor_site = (AllocationSite*)evictor;
      if (site->evicted_by.count(evictor_site) == 0)
         site->evicted_by[evictor_site] = 0;
      site->evicted_by[evictor_site]++;
   }
}

MemoryTracker::RoutineTracer::RoutineTracer()
{
   Sim()->setMemoryTracker(new MemoryTracker());
}

MemoryTracker::RoutineTracer::~RoutineTracer()
{
   delete Sim()->getMemoryTracker();
}

void MemoryTracker::RoutineTracer::addRoutine(IntPtr eip, const char *name, const char *imgname, IntPtr offset, int column, int line, const char *filename)
{
   ScopedLock sl(m_lock);

   if (m_routines.count(eip) == 0)
   {
      m_routines[eip] = new RoutineTracer::Routine(eip, name, imgname, offset, column, line, filename);
   }
}

bool MemoryTracker::RoutineTracer::hasRoutine(IntPtr eip)
{
   ScopedLock sl(m_lock);

   return m_routines.count(eip) > 0;
}
