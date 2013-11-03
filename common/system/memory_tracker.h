#ifndef __MEMORY_TRACKER_H
#define __MEMORY_TRACKER_H

#include "fixed_types.h"
#include "lock.h"
#include "routine_tracer.h"
#include "cache_efficiency_tracker.h"

#include <vector>
#include <map>
#include <unordered_map>

// Define to add a slow checker for finding allocation sites by address
//#define ASSERT_FIND_OWNER

class MemoryTracker
{
   public:
      // A light routine tracer that will activate the RoutineTracer infrastructure and allow us to get call stacks
      class RoutineTracerThread : public ::RoutineTracerThread
      {
         public:
            RoutineTracerThread(Thread *thread) : ::RoutineTracerThread(thread) {}
            const CallStack& getCallsiteStack() const { return m_callsite_stack; }
         protected:
            virtual void functionEnter(IntPtr eip, IntPtr callEip);
            virtual void functionExit(IntPtr eip);
            virtual void functionChildEnter(IntPtr eip, IntPtr eip_child) {}
            virtual void functionChildExit(IntPtr eip, IntPtr eip_child) {}
         private:
            CallStack m_callsite_stack;
      };
      class RoutineTracer : public ::RoutineTracer
      {
         public:
            RoutineTracer();
            virtual ~RoutineTracer();

            virtual RoutineTracerThread* getThreadHandler(Thread *thread) { return new RoutineTracerThread(thread); }
            virtual void addRoutine(IntPtr eip, const char *name, const char *imgname, IntPtr offset, int column, int line, const char *filename);
            virtual bool hasRoutine(IntPtr eip);

            virtual const Routine* getRoutineInfo(IntPtr eip) { return m_routines.count(eip) ? m_routines[eip] : NULL; }

         private:
            Lock m_lock;
            typedef std::unordered_map<IntPtr, RoutineTracer::Routine*> RoutineMap;
            RoutineMap m_routines;
      };

      MemoryTracker();
      ~MemoryTracker();

      void logMalloc(thread_id_t thread_id, UInt64 eip, UInt64 address, UInt64 size);
      void logFree(thread_id_t thread_id, UInt64 eip, UInt64 address);

   private:
      struct AllocationSite
      {
         AllocationSite()
            : num_allocations(0), total_size(0), total_loads(0), total_stores(0)
            , hit_where_load(HitWhere::NUM_HITWHERES, 0)
            , hit_where_store(HitWhere::NUM_HITWHERES, 0)
         {}
         UInt64 num_allocations;
         UInt64 total_size;
         UInt64 total_loads, total_stores;
         std::vector<UInt64> hit_where_load, hit_where_store;
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

      #ifdef ASSERT_FIND_OWNER
         std::unordered_map<UInt64, AllocationSite*> m_allocations_slow;
      #endif

      UInt64 ce_get_owner(core_id_t core_id, UInt64 address);
      void ce_notify_access(UInt64 owner, Core::mem_op_t mem_op_type, HitWhere::where_t hit_where);
      void ce_notify_evict(bool on_roi_end, UInt64 owner, UInt64 evictor, CacheBlockInfo::BitsUsedType bits_used, UInt32 bits_total);

      static UInt64 __ce_get_owner(UInt64 user, core_id_t core_id, UInt64 address)
      { return ((MemoryTracker*)user)->ce_get_owner(core_id, address); }
      static void __ce_notify_access(UInt64 user, UInt64 owner, Core::mem_op_t mem_op_type, HitWhere::where_t hit_where)
      { ((MemoryTracker*)user)->ce_notify_access(owner, mem_op_type, hit_where); }
      static void __ce_notify_evict(UInt64 user, bool on_roi_end, UInt64 owner, UInt64 evictor, CacheBlockInfo::BitsUsedType bits_used, UInt32 bits_total)
      { ((MemoryTracker*)user)->ce_notify_evict(on_roi_end, owner, evictor, bits_used, bits_total); }
};

#endif // __MEMORY_TRACKER_H
