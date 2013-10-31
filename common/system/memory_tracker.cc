#include "memory_tracker.h"
#include "simulator.h"
#include "thread_manager.h"
#include "thread.h"

MemoryTracker::MemoryTracker()
{
   LOG_ASSERT_ERROR(Sim()->getRoutineTracer() != NULL, "MemoryTracker needs a routine tracer to be active");
}

MemoryTracker::~MemoryTracker()
{
   for(auto it = m_allocation_sites.begin(); it != m_allocation_sites.end(); ++it)
   {
      const CallStack &stack = it->first;
      const AllocationSite &site = it->second;
      printf("Allocation site %ld:\n", site.site_id);
      printf("\tCall stack:");
      for(auto jt = stack.begin(); jt != stack.end(); ++jt)
      {
         const RoutineTracer::Routine *rtn = Sim()->getRoutineTracer()->getRoutineInfo(*jt);
         if (rtn)
            printf("\t\t[%lx] %s\n", *jt, rtn->m_name);
         else
            printf("\t\t[%lx] ???\n", *jt);
      }
      printf("\tTotal allocated: %ld bytes\n", site.total_size);
   }
}

void MemoryTracker::logMalloc(thread_id_t thread_id, UInt64 eip, UInt64 address, UInt64 size)
{
   ScopedLock sl(m_lock);

   CallStack stack = Sim()->getThreadManager()->getThreadFromID(thread_id)->getRoutineTracer()->getCallStack();
   stack.push_back(eip);

   UInt64 site_id;
   AllocationSites::iterator it = m_allocation_sites.find(stack);
   if (it != m_allocation_sites.end())
      site_id = it->second.site_id;
   else
   {
      site_id = m_allocation_sites.size();
      m_allocation_sites[stack] = AllocationSite(site_id);
   }

   printf("memtracker: site %ld(%lx) malloc %lx + %lx\n", site_id, eip, address, size);

   m_allocation_sites[stack].total_size += size;
   m_allocations[address] = Allocation(size, site_id);
}

void MemoryTracker::logFree(thread_id_t thread_id, UInt64 eip, UInt64 address)
{
   ScopedLock sl(m_lock);

   printf("memtracker: free %lx\n", address);

   auto it = m_allocations.find(address);
   if (it != m_allocations.end())
      m_allocations.erase(it);
}
