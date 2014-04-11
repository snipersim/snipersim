#ifndef __FAST_CACHE_H
#define __FAST_CACHE_H

#include "memory_manager_fast.h"

namespace FastNehalem
{
   class Dram : public CacheBase
   {
      private:
         const ComponentLatency m_latency;
         UInt64 m_reads, m_writes;
         SubsecondTime m_total_latency;
      public:
         Dram(Core *core, String name, UInt64 latency)
            : m_latency(core->getDvfsDomain(), latency)
         {
            m_reads = m_writes = 0;
            registerStatsMetric(name, core->getId(), "reads", &m_reads);
            registerStatsMetric(name, core->getId(), "writes", &m_writes);
            m_total_latency = SubsecondTime::Zero();
            registerStatsMetric(name, core->getId(), "total-access-latency", &m_total_latency);
         }
         SubsecondTime access(Core::mem_op_t mem_op_type, IntPtr tag)
         {
            if (mem_op_type == Core::WRITE)
               ++m_writes;
            else
               ++m_reads;
            m_total_latency += m_latency.getLatency();
            return m_latency.getLatency();
         }
   };

   template <UInt32 assoc>
   class CacheSet
   {
      private:
         IntPtr m_tags[assoc];
         UInt64 m_lru[assoc];
         UInt64 m_lru_max;
      public:
         bool find(IntPtr tag)
         {
            for(unsigned int idx = 0; idx < assoc; ++idx)
            {
               if (m_tags[idx] == tag)
               {
                  m_lru[idx] = ++m_lru_max;
                  return true;
               }
            }
            // Find replacement
            UInt64 lru_min = UINT64_MAX; unsigned int idx_min = 0;
            for(unsigned int idx = 0; idx < assoc; ++idx)
            {
               if (m_lru[idx] < lru_min)
               {
                  lru_min = m_lru[idx];
                  idx_min = idx;
               }
            }
            m_tags[idx_min] = tag;
            m_lru[idx_min] = ++m_lru_max;
            return false;
         }
   };

   template <UInt32 assoc, UInt32 size_kb>
   class Cache : public CacheBase
   {
      private:
         const MemComponent::component_t m_mem_component;
         const ComponentLatency m_latency;
         CacheBase* const m_next_level;
         const UInt64 m_num_sets;
         const IntPtr m_sets_mask;
         std::vector<CacheSet<assoc> > m_sets;
         UInt64 m_loads, m_stores, m_load_misses, m_store_misses;

      public:
         Cache(Core *core, String name, MemComponent::component_t mem_component, UInt64 latency, CacheBase* next_level)
            : m_mem_component(mem_component)
            , m_latency(core->getDvfsDomain(), latency)
            , m_next_level(next_level)
            , m_num_sets(size_kb * 1024 / 64 / assoc)
            , m_sets_mask(m_num_sets - 1)
            , m_sets(m_num_sets)
         {
            LOG_ASSERT_ERROR(UInt64(1) << floorLog2(m_num_sets) == m_num_sets, "Number of sets must be power of 2");
            m_loads = m_stores = m_load_misses = m_store_misses = 0;
            registerStatsMetric(name, core->getId(), "loads", &m_loads);
            registerStatsMetric(name, core->getId(), "stores", &m_stores);
            registerStatsMetric(name, core->getId(), "load-misses", &m_load_misses);
            registerStatsMetric(name, core->getId(), "store-misses", &m_store_misses);
         }
         virtual ~Cache() {}

         SubsecondTime access(Core::mem_op_t mem_op_type, IntPtr tag)
         {
            if (mem_op_type == Core::WRITE) ++m_stores; else ++m_loads;
            if (m_sets[tag & m_sets_mask].find(tag))
               return m_latency.getLatency();
            else
            {
               if (mem_op_type == Core::WRITE) ++m_store_misses; else ++m_load_misses;
               return m_next_level->access(mem_op_type, tag);
            }
         }
   };

   template <UInt32 assoc, UInt32 size_kb>
   class CacheLocked : public Cache<assoc, size_kb>
   {
      private:
         Lock lock;
      public:
         CacheLocked(Core *core, String name, MemComponent::component_t mem_component, UInt64 latency, CacheBase* next_level)
            : Cache<assoc, size_kb>(core, name, mem_component, latency, next_level)
         {}
         SubsecondTime access(Core::mem_op_t mem_op_type, IntPtr tag)
         {
            ScopedLock sl(lock);
            return Cache<assoc, size_kb>::access(mem_op_type, tag);
         }
   };
}

#endif // __FAST_CACHE_H
