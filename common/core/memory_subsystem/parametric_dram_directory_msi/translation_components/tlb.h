#ifndef TLB_H
#define TLB_H

#include "fixed_types.h"
#include "cache.h"
#include <util.h>
#include <unordered_map>
#include "pagetable.h"
#include "lock.h"
#include "hash_map_set.h"
#include <vector>
#include <queue>
#include <memory>
#include "trans_defs.h"
#include "tlb_prefetcher_base.h"
#include "sim_log.h"

namespace ParametricDramDirectoryMSI
{
	/**
	 * @brief Result of a TLB allocation operation.
	 * 
	 * When a new translation is allocated to the TLB, this struct captures
	 * whether an eviction occurred and the details of the evicted entry.
	 */
	struct TLBAllocResult
	{
		bool evicted;          ///< True if an existing entry was evicted
		IntPtr address;        ///< Virtual address of the evicted entry (if evicted)
		int page_size;         ///< Page size in bits of the evicted entry
		IntPtr ppn;            ///< Physical page number of the evicted entry

		TLBAllocResult() : evicted(false), address(0), page_size(0), ppn(0) {}
		
		TLBAllocResult(bool evicted, IntPtr address, int page_size, IntPtr ppn)
			: evicted(evicted), address(address), page_size(page_size), ppn(ppn) {}
	};

	/**
	 * @brief Represents a translation that was evicted from a TLB.
	 * 
	 * Used to track translations that need to be propagated to lower-level TLBs.
	 */
	struct EvictedTranslation
	{
		IntPtr address;        ///< Virtual address of the translation
		int page_size;         ///< Page size in bits
		IntPtr ppn;            ///< Physical page number

		EvictedTranslation() : address(0), page_size(0), ppn(0) {}
		
		EvictedTranslation(IntPtr address, int page_size, IntPtr ppn)
			: address(address), page_size(page_size), ppn(ppn) {}
	};
	
	enum TLBtype
	{
		Instruction,
		Data,
		Unified
	};

	class TLB
	{
	private:
		UInt32 m_size;
		core_id_t m_core_id;
		String m_name;
		UInt32 m_associativity;
		UInt32 m_num_entries;
		UInt32 m_num_sets;
		UInt32 entry_size;

		Cache m_cache;
		String m_type;
		TLBPrefetcherBase **prefetchers;
		int number_of_prefetchers;
		//make m_page_size_list a unique pointer
		
		std::unique_ptr<int[]> m_page_size_list;

		int m_page_sizes;
		bool m_allocate_miss;
		bool m_prefetch;
		int max_prefetch_count;

		
		ComponentLatency m_access_latency;
		
		std::priority_queue<query_entry, std::vector<query_entry>, Compare> entry_priority_queue;
		std::unordered_map<uint64_t, uint32_t> m_pq_region_refcount;  // Region ID → number of PQ entries (for dedup)

		// External observers notified on TLB evictions (e.g., PQ prefetchers
		// registered by the TLB subsystem so that main-TLB evictions reach
		// eviction-aware prefetchers like the recency prefetcher).
		std::vector<TLBPrefetcherBase*> m_victim_observers;

		struct TLBStats
		{
			
			UInt64 m_access, m_hit, m_miss, m_insertions, m_eviction;

			// Instruction vs Data breakdown (meaningful for Unified TLBs)
			UInt64 m_access_instruction, m_access_data;
			UInt64 m_hit_instruction, m_hit_data;
			UInt64 m_miss_instruction, m_miss_data;
			UInt64 m_insertions_instruction, m_insertions_data;
			UInt64 m_eviction_instruction, m_eviction_data;
			UInt64 m_pq_dedup_skipped;  // Prefetches skipped due to PQ region dedup

		} tlb_stats;

		SimLog *tlb_log;

	public:
		TLB(String name, String cfgname, core_id_t core_id, ComponentLatency access_latency, UInt32 num_entries, UInt32 associativity, int *page_size_list, int page_sizes, String tlb_type, bool allocate_on_miss, bool prefetch = false, TLBPrefetcherBase **tpb = NULL, int number_of_prefetchers = 0, int max_prefetch_count = 1000);
		CacheBlockInfo *lookup(IntPtr address, SubsecondTime now, bool model_count, Core::lock_signal_t lock, IntPtr eip, bool modeled, bool count, PageTable *pt, bool instruction = false);
		// Returns: TLBAllocResult with eviction flag, evicted address, evicted page size, evicted PPN
		TLBAllocResult allocate(IntPtr address, SubsecondTime now, bool count, Core::lock_signal_t lock, int page_size, IntPtr ppn, bool self_alloc = false, bool instruction = false);
		TLBtype getType() { return (m_type == "Instruction") ? Instruction : (m_type == "Data") ? Data
																								: Unified; };
		String getName() { return m_name; };
		Cache& getCache() { return m_cache; };
		int getAssoc() { return m_associativity; };
		bool getAllocateOnMiss() { return m_allocate_miss; };
		bool getPrefetch() { return m_prefetch; };
		TLBPrefetcherBase** getPrefetchers() { return prefetchers; };
		int getNumPrefetchers() { return number_of_prefetchers; };
		void addVictimObserver(TLBPrefetcherBase* observer) { m_victim_observers.push_back(observer); }
		int getEntrySize() { return entry_size; };
		SubsecondTime getLatency() { return m_access_latency.getLatency(); };
		bool supportsPageSize(int page_size)
		{
			for (int i = 0; i < m_page_sizes; i++)
			{
				if (m_page_size_list[i] == page_size)
					return true;
			}
			return false;
		}
		
		/**
		 * @brief Invalidate a TLB entry for a given virtual address
		 * 
		 * Used for TLB shootdown after page migration. Invalidates the entry
		 * matching the given virtual address and page size.
		 * 
		 * @param address Virtual address to invalidate
		 * @param page_size Page size in bits (e.g., 12 for 4KB, 21 for 2MB)
		 * @return True if an entry was found and invalidated
		 */
		bool invalidate(IntPtr address, int page_size);
		
		/**
		 * @brief Check if TLB contains an entry for a given virtual address
		 * 
		 * Used for sanity checks after TLB shootdown. Does NOT update stats or LRU.
		 * 
		 * @param address Virtual address to check
		 * @param page_size Page size in bits (e.g., 12 for 4KB, 21 for 2MB)
		 * @return True if an entry exists for this address
		 */
		bool contains(IntPtr address, int page_size) const;
		
		~TLB();
	};

}

#endif // TLB_H
