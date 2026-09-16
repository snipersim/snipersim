#include "tlb.h"
#include "stats.h"
#include "config.hpp"
#include "simulator.h"
#include <cmath>
#include "cache_cntlr.h"
#include <iostream>
#include <utility>
#include "memory_manager.h"
#include "core_manager.h"
#include "cache_set.h"
#include "cache_base.h"
#include "utils.h"
#include "log.h"
#include "rng.h"
#include "address_home_lookup.h"
#include "fault_injection.h"
#include "memory_manager.h"
#include "debug_config.h"
#include <cstdlib>


namespace ParametricDramDirectoryMSI
{

    TLB::TLB(String name, String cfgname, core_id_t core_id, ComponentLatency access_latency, UInt32 num_entries, UInt32 associativity, int *page_size_list, int page_sizes, String tlb_type, bool allocate_on_miss, bool prefetch, TLBPrefetcherBase **tpb, int _number_of_prefetchers, int _max_prefetch_count)
        : m_size(num_entries),
          m_core_id(core_id),
          m_name(name),
          m_associativity(associativity),
          m_num_entries(num_entries),
          m_num_sets(num_entries / associativity),
          entry_size(1L << 3),
          m_cache(name + "_cache",
                  cfgname,
                  core_id, num_entries / associativity,
                  associativity, entry_size,
                  Sim()->getCfg()->hasKey(cfgname + "/replacement_policy")
                     ? Sim()->getCfg()->getString(cfgname + "/replacement_policy") : "lru",
                  CacheBase::PR_L1_CACHE, CacheBase::HASH_MOD,
                  // HASH_MOD rather than HASH_MASK so a TLB may have a set count
                  // that is not a power of two -- real TLB geometries often are
                  // not (48, 96, 1280, 3072 entries).  HASH_MASK's & (n-1) is
                  // only equivalent to % n when n is a power of two; for the
                  // power-of-two TLBs configured here the two are identical and
                  // the compiler emits the same mask.
                  NULL,
                  NULL, true, page_size_list, page_sizes),
          m_type(tlb_type),
          prefetchers(tpb),
          number_of_prefetchers(_number_of_prefetchers),
          m_page_size_list(nullptr),
          m_page_sizes(page_sizes),
          m_allocate_miss(allocate_on_miss),
          m_prefetch(prefetch),
          max_prefetch_count(_max_prefetch_count),
          m_access_latency(access_latency)
    {
        // Initialize SimLog for TLB (uses DEBUG_TLB flag)
        tlb_log = new SimLog(m_name.c_str(), core_id, DEBUG_TLB);


        LOG_ASSERT_ERROR((num_entries / associativity) * associativity == num_entries, "Invalid TLB configuration: num_entries(%d) must be a multiple of the associativity(%d)", num_entries, associativity);

        std::cout << "[MMU] Instantiating TLB: " << m_name << " "
                  << " Core ID: " << m_core_id << " "
                  << " Stores: " << tlb_type << " "
                  << " Number of entries: " << m_size << " "
                  << " Associativity: " << m_associativity << " "
                  << " TLB Type: " << m_type << " "
                  << " Allocate on miss: " << (m_allocate_miss ? "true" : "false") << " "
                  << " Number of prefetchers: " << number_of_prefetchers << " "
                  << " Access latency: " << m_access_latency.getLatency().getNS() << "ns "
                  << " Page sizes: " << m_page_sizes << std::endl;

        m_page_size_list = std::unique_ptr<int[]>(new int[m_page_sizes]);

        for (int i = 0; i < m_page_sizes; i++)
        {
            m_page_size_list[i] = page_size_list[i];
        }

        bzero(&tlb_stats, sizeof(tlb_stats));


		m_num_sets = num_entries / associativity;
		entry_size = ceil(((48 - log2(m_num_sets) - log2(associativity)) + 52)/8);

		bzero(&tlb_stats, sizeof(tlb_stats));

        registerStatsMetric(name, core_id, "accesses", &tlb_stats.m_access);
        registerStatsMetric(name, core_id, "hits", &tlb_stats.m_hit);
        registerStatsMetric(name, core_id, "misses", &tlb_stats.m_miss);
        registerStatsMetric(name, core_id, "evictions", &tlb_stats.m_eviction);
        registerStatsMetric(name, core_id, "insertions", &tlb_stats.m_insertions);

        // Register instruction vs data breakdown stats for Unified TLBs
        if (tlb_type == "Unified")
        {
            registerStatsMetric(name, core_id, "accesses_instruction", &tlb_stats.m_access_instruction);
            registerStatsMetric(name, core_id, "accesses_data", &tlb_stats.m_access_data);
            registerStatsMetric(name, core_id, "hits_instruction", &tlb_stats.m_hit_instruction);
            registerStatsMetric(name, core_id, "hits_data", &tlb_stats.m_hit_data);
            registerStatsMetric(name, core_id, "misses_instruction", &tlb_stats.m_miss_instruction);
            registerStatsMetric(name, core_id, "misses_data", &tlb_stats.m_miss_data);
            registerStatsMetric(name, core_id, "evictions_instruction", &tlb_stats.m_eviction_instruction);
            registerStatsMetric(name, core_id, "evictions_data", &tlb_stats.m_eviction_data);
            registerStatsMetric(name, core_id, "insertions_instruction", &tlb_stats.m_insertions_instruction);
            registerStatsMetric(name, core_id, "insertions_data", &tlb_stats.m_insertions_data);
        }

        if (m_prefetch)
        {
            registerStatsMetric(name, core_id, "pq_dedup_skipped", &tlb_stats.m_pq_dedup_skipped);
        }

    }

    CacheBlockInfo *TLB::lookup(IntPtr address, SubsecondTime now, bool model_count, Core::lock_signal_t lock_signal, IntPtr eip, bool modeled, bool count, PageTable *pt, bool instruction)
    {

        if (m_prefetch)
        {
            tlb_log->debug("Prefetching enabled at time: ", now.getNS(), " ns");

            // Materialize any prefetched translations whose walks have completed
            while (!entry_priority_queue.empty() && entry_priority_queue.top().timestamp <= now)
            {
                query_entry entry = entry_priority_queue.top();
                entry_priority_queue.pop();
                // Decrement region refcount on materialization
                uint64_t region_id = static_cast<uint64_t>(entry.address) >> 15;  // 32KB region
                auto it = m_pq_region_refcount.find(region_id);
                if (it != m_pq_region_refcount.end()) {
                    if (--it->second == 0)
                        m_pq_region_refcount.erase(it);
                }
                tlb_log->debug("Materializing prefetch for address: ", entry.address, " at time: ", now.getNS(), " ns");
                allocate(entry.address, entry.timestamp, false, lock_signal, entry.page_size, entry.ppn, true);
            }
        }

        if (model_count)
        {
            tlb_stats.m_access++;
            if (getType() == TLBtype::Unified)
            {
                if (instruction)
                    tlb_stats.m_access_instruction++;
                else
                    tlb_stats.m_access_data++;
            }
        }

        tlb_log->debug("Lookup for address: ", address, " at time: ", now.getNS(), " ns");

        CacheBlockInfo *hit = m_cache.accessSingleLineTLB(address, Cache::LOAD, NULL, 0, now, true);

        // Detect whether the hit came from a prefetch-queue-sourced entry.
        // PQ-materialized entries are tagged with CacheBlockInfo::PREFETCH in allocate().
        // Clear the flag on demand consumption so the entry looks normal afterwards.
        bool pq_hit = false;
        if (hit && hit->hasOption(CacheBlockInfo::PREFETCH))
        {
            pq_hit = true;
            hit->clearOption(CacheBlockInfo::PREFETCH);
        }

        if (hit)
            tlb_log->debug("Hit at time: ", now.getNS(), " ns", pq_hit ? " (PQ)" : "");
        if (!hit)
            tlb_log->debug("Miss at time: ", now.getNS(), " ns");

        // Invoke prefetchers on EVERY access (hits + misses) so that:
        //   1. Timeliness tracking sees all demand accesses
        //   2. Predictions are generated based on hit type (miss / PQ hit / regular hit)
        if (m_prefetch && prefetchers != NULL && pt != NULL)
        {
            tlb_log->debug("Generating prefetches at time: ", now.getNS(), " ns");

            size_t current_prefetches = entry_priority_queue.size();
            for (int i = 0; i < number_of_prefetchers && current_prefetches < static_cast<size_t>(max_prefetch_count); i++)
            {
                tlb_log->debug("Using prefetcher ", i, " at time: ", now.getNS(), " ns");

                // 0 means unknown: on a miss there is no entry to read a page size from,
                // and the prefetcher discovers it during the walk.
                int hit_page_size = hit ? hit->getPageSize() : 0;
                std::vector<query_entry> generated_prefetches = prefetchers[i]->performPrefetch(address, eip, lock_signal, modeled, count, pt, instruction, /*tlb_hit=*/(hit != NULL), /*pq_hit=*/pq_hit, /*page_size=*/hit_page_size);
                tlb_log->debug("Prefetcher ", i, " generated ", generated_prefetches.size(), " prefetches at time: ", now.getNS(), " ns");

                // PQ dedup: check at region granularity BEFORE inserting the batch.
                // All PTEs from a single prediction share the same region (1 PTW → 8 PTEs).
                // Only skip if this region was already pending from a prior prediction.
                bool batch_deduped = false;
                if (!generated_prefetches.empty())
                {
                    for (auto &first_valid : generated_prefetches)
                    {
                        if (first_valid.ppn == 0) continue;
                        uint64_t region_id = static_cast<uint64_t>(first_valid.address) >> 15;
                        if (m_pq_region_refcount.count(region_id) > 0)
                        {
                            tlb_stats.m_pq_dedup_skipped++;
                            batch_deduped = true;
                        }
                        break;  // only need to check the first valid entry
                    }
                }

                if (!batch_deduped)
                {
                    for (auto &pref : generated_prefetches)
                    {
                        if (pref.ppn == 0)
                            continue;

                        tlb_log->debug("Adding prefetch for address: ", pref.address, " at time: ", now.getNS(), " ns");

                        if (current_prefetches >= static_cast<size_t>(max_prefetch_count))
                            break;
                        uint64_t region_id = static_cast<uint64_t>(pref.address) >> 15;
                        entry_priority_queue.push(pref);
                        m_pq_region_refcount[region_id]++;
                        current_prefetches++;
                    }
                }
            }
        }

        if (hit)
        {
            tlb_stats.m_hit++;
            if (getType() == TLBtype::Unified)
            {
                if (instruction)
                    tlb_stats.m_hit_instruction++;
                else
                    tlb_stats.m_hit_data++;
            }
            return hit;
        }

        if (model_count)
        {
            tlb_stats.m_miss++; // We reach this point if L1 TLB Miss
            if (getType() == TLBtype::Unified)
            {
                if (instruction)
                    tlb_stats.m_miss_instruction++;
                else
                    tlb_stats.m_miss_data++;
            }
        }

        return NULL;
    }

    TLBAllocResult TLB::allocate(IntPtr address, SubsecondTime now, bool count, Core::lock_signal_t lock_signal, int page_size, IntPtr ppn, bool self_alloc, bool instruction)
    {
        if (getPrefetch() && !self_alloc)
        {
            return TLBAllocResult(false, 0, 0, 0);
        }
        IntPtr evict_addr;
        CacheBlockInfo evict_block_info;

        IntPtr tag;
        UInt32 set_index;

        m_cache.splitAddressTLB(address, tag, set_index, page_size);

        tlb_log->debug("Allocate ", address, " at level: ", m_name.c_str(), " with page_size ", page_size, " and tag ", tag);

        bool eviction = false;
        m_cache.insertSingleLineTLB(address, NULL, &eviction, &evict_addr, &evict_block_info, NULL, now, NULL, CacheBlockInfo::block_type_t::DATA, page_size, ppn);

        // Mark prefetch-queue-sourced entries so lookup() can detect PQ hits
        if (self_alloc)
        {
            CacheBlockInfo *inserted = m_cache.accessSingleLineTLB(address, Cache::LOAD, NULL, 0, now, false);
            if (inserted)
                inserted->setOption(CacheBlockInfo::PREFETCH);
        }

        if(count || self_alloc)
        {
            tlb_stats.m_insertions++;
            if (getType() == TLBtype::Unified)
            {
                if (instruction)
                    tlb_stats.m_insertions_instruction++;
                else
                    tlb_stats.m_insertions_data++;
            }
        }

        if (eviction && (count || self_alloc))
        {
            tlb_stats.m_eviction++;
            if (getType() == TLBtype::Unified)
            {
                if (instruction)
                    tlb_stats.m_eviction_instruction++;
                else
                    tlb_stats.m_eviction_data++;
            }
        }

        if (eviction)
            tlb_log->debug("Evicted ", evict_addr, " from level: ", m_name.c_str(), " with page_size ", page_size);

        // Notify this TLB's own prefetchers about the evicted victim so they
        // can maintain eviction-aware structures (e.g., the recency list).
        if (eviction && prefetchers != NULL)
        {
            for (int i = 0; i < number_of_prefetchers; i++)
                prefetchers[i]->notifyVictim(evict_addr, evict_block_info.getPageSize(), evict_block_info.getPPN());
        }

        // Notify external victim observers (PQ prefetchers wired by the
        // TLB subsystem via addVictimObserver).  This is the primary path
        // for regular (non-PQ) TLBs whose evictions must reach PQ
        // prefetchers that use victim-based tracking.
        if (eviction && !m_victim_observers.empty())
        {
            for (auto *obs : m_victim_observers)
                obs->notifyVictim(evict_addr, evict_block_info.getPageSize(), evict_block_info.getPPN());
        }

        return TLBAllocResult(eviction, evict_addr, evict_block_info.getPageSize(), evict_block_info.getPPN());
    }

    bool TLB::invalidate(IntPtr address, int page_size)
    {
        // Use the TLB-specific invalidation method that uses splitAddressTLB
        bool invalidated = m_cache.invalidateSingleLineTLB(address, page_size);
        
        if (invalidated)
        {
            tlb_log->debug("Invalidated entry for address ", address, " page_size ", page_size);
        }
        
        return invalidated;
    }

    bool TLB::contains(IntPtr address, int page_size) const
    {
        // Check if entry exists without modifying anything (for sanity checks)
        return m_cache.containsTLB(address, page_size);
    }

    TLB::~TLB()
    {
        delete tlb_log;
        
        if (prefetchers != NULL)
        {
            for (int i = 0; i < number_of_prefetchers; i++)
            {
                delete prefetchers[i];
            }
            free(prefetchers);
            prefetchers = NULL;
        }
    }

}
