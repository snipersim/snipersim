#include "memory_tracker.h"
#include "simulator.h"
#include "thread_manager.h"
#include "thread.h"

#include <unordered_set>

MemoryTracker::MemoryTracker()
{
   Sim()->getConfig()->setCacheEfficiencyCallbacks(__ce_get_owner, __ce_notify_access, __ce_notify_evict, (UInt64)this);
}

MemoryTracker::~MemoryTracker()
{
   FILE *fp = fopen(Sim()->getConfig()->formatOutputFileName("sim.memorytracker").c_str(), "w");
   std::unordered_set<UInt64> sites_printed;

   fprintf(fp, "W\t");
   for(int h = HitWhere::WHERE_FIRST ; h < HitWhere::NUM_HITWHERES ; h++)
      if (HitWhereIsValid((HitWhere::where_t)h))
         fprintf(fp, "%s,", HitWhereString((HitWhere::where_t)h));
   fprintf(fp, "\n");

   for(auto it = m_allocation_sites.begin(); it != m_allocation_sites.end(); ++it)
   {
      const CallStack &stack = it->first;
      const AllocationSite *site = it->second;

      if (site->total_loads + site->total_stores)
      {
         for(auto jt = stack.begin(); jt != stack.end(); ++jt)
         {
            if (sites_printed.count(*jt) == 0)
            {
               const RoutineTracer::Routine *rtn = Sim()->getRoutineTracer()->getRoutineInfo(*jt);
               if (rtn)
                  fprintf(fp, "F\t%" PRIxPTR "\t%s\t%s\n", *jt, rtn->m_name, rtn->m_location);
               sites_printed.insert(*jt);
            }
         }
         fprintf(fp, "S\t%lx\t", (unsigned long)site);
         for(auto jt = stack.begin(); jt != stack.end(); ++jt)
            fprintf(fp, ":%" PRIxPTR, *jt);
         fprintf(fp, "\tnum-allocations=%" PRId64, site->num_allocations);
         fprintf(fp, "\ttotal-allocated=%" PRId64, site->total_size);
         fprintf(fp, "\thit-where=");
         for(int h = HitWhere::WHERE_FIRST ; h < HitWhere::NUM_HITWHERES ; h++)
         {
            if (HitWhereIsValid((HitWhere::where_t)h) && site->hit_where_load[h] + site->hit_where_store[h] > 0)
            {
               fprintf(fp, "L%s:%" PRId64 ",", HitWhereString((HitWhere::where_t)h), site->hit_where_load[h]);
               fprintf(fp, "S%s:%" PRId64 ",", HitWhereString((HitWhere::where_t)h), site->hit_where_store[h]);
            }
         }
         if (site->evicted_by.size())
         {
            fprintf(fp, "\tevicted-by=");
            for(auto it = site->evicted_by.begin(); it != site->evicted_by.end(); ++it)
               fprintf(fp, "%lx:%" PRId64 ",", (unsigned long)it->first, it->second);
         }
         fprintf(fp, "\n");
      }
   }
}

void MemoryTracker::logMalloc(thread_id_t thread_id, UInt64 eip, UInt64 address, UInt64 size)
{
   ScopedLock sl(m_lock);

   ::RoutineTracerThread *tracer = Sim()->getThreadManager()->getThreadFromID(thread_id)->getRoutineTracer();
   const CallStack stack = dynamic_cast<MemoryTracker::RoutineTracerThread*>(tracer)->getCallsiteStack();

   AllocationSite *site = NULL;
   AllocationSites::iterator it = m_allocation_sites.find(stack);
   if (it != m_allocation_sites.end())
      site = it->second;
   else
   {
      site = new AllocationSite();
      m_allocation_sites[stack] = site;
   }

   // Store the first address of the first cache line that no longer belongs to the allocation
   UInt64 lower = address & ~63, upper = (address + size + 63) & ~63;

   //printf("memtracker: site %p(%lx) malloc %lx + %10lx (%lx .. %lx)\n", site, eip, address, size, lower, upper);

   auto previous = m_allocations.upper_bound(lower);
   if (previous != m_allocations.end() && lower >= previous->first - previous->second.size)
   {
      //printf("\t%p overwriting %p\n", site, previous->second.site);
      if (previous->first - previous->second.size < lower)
      {
         UInt64 start = previous->first - previous->second.size;
         m_allocations[lower] = Allocation(lower - start, previous->second.site);
         //printf("\tremain %p %lx .. %lx\n", previous->second.site, start, lower);
      }
      if (previous->first > upper)
      {
         m_allocations[previous->first] = Allocation(previous->first - upper, previous->second.site);
         //printf("\tremain %p %lx .. %lx\n", previous->second.site, upper, previous->first);
      }
      else
      {
         m_allocations.erase(previous);
      }
   }

   m_allocations[upper] = Allocation(upper - lower, site);

   #ifdef ASSERT_FIND_OWNER
      for(UInt64 addr = lower; addr < upper; addr += 64)
         m_allocations_slow[addr] = site;
   #endif

   m_allocation_sites[stack]->num_allocations++;
   m_allocation_sites[stack]->total_size += size;
}

void MemoryTracker::logFree(thread_id_t thread_id, UInt64 eip, UInt64 address)
{
   ScopedLock sl(m_lock);

   //printf("memtracker: free %lx\n", address);
}

UInt64 MemoryTracker::ce_get_owner(core_id_t core_id, UInt64 address)
{
   ScopedLock sl(m_lock);
   AllocationSite *owner = NULL;

   // upper_bound returns the first entry greater than address
   // Because the key in m_allocations is the first cache line that no longer falls into the range,
   // we will find the correct alloction *if* address falls within it
   auto upper = m_allocations.upper_bound(address);
   if (upper != m_allocations.end() && address >= upper->first - upper->second.size)
      owner = upper->second.site;

   #ifdef ASSERT_FIND_OWNER
      AllocationSite *owner_slow = (m_allocations_slow.count(address & ~63) == 0) ? NULL : m_allocations_slow[address & ~63];
      LOG_ASSERT_WARNING(owner == owner_slow, "ASSERT_FIND_OWNER: owners for %lx don't match (fast %p != slow %p)", address, owner, owner_slow);
   #endif

   return (UInt64)owner;
}

void MemoryTracker::ce_notify_access(UInt64 owner, Core::mem_op_t mem_op_type, HitWhere::where_t hit_where)
{
   if (owner)
   {
      AllocationSite *site = (AllocationSite*)owner;
      if (mem_op_type == Core::WRITE)
      {
         site->total_stores++;
         site->hit_where_store[hit_where]++;
      }
      else
      {
         site->total_loads++;
         site->hit_where_load[hit_where]++;
      }
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

void MemoryTracker::RoutineTracerThread::functionEnter(IntPtr eip, IntPtr callEip)
{
   m_callsite_stack.push_back(callEip);
}

void MemoryTracker::RoutineTracerThread::functionExit(IntPtr eip)
{
   m_callsite_stack.pop_back();
}
