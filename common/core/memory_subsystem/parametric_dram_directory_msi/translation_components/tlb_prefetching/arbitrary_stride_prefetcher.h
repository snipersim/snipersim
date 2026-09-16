
#pragma once
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "shmem_perf_model.h"
#include "pagetable.h"
#include "tlb_subsystem.h"
#include "cache_block_info.h"
#include "tlb_prefetcher_base.h"
#include <fstream>
#include <string>
#include <vector>
#include <cstring>

namespace ParametricDramDirectoryMSI
{

	class ArbitraryStridePrefetcher : public TLBPrefetcherBase
	{

	protected:
		typedef struct entry_prefetcher_t
		{
			IntPtr PC;
			IntPtr vaddr;
			long long stride;
			uint saturation_counter;
		} entry_prefetcher;

		struct ASPStats
		{
			// Existing
			UInt64 successful_prefetches;
			UInt64 prefetch_attempts;
			UInt64 failed_prefetches;

			// New high-level effectiveness metrics
			UInt64 queries;                // Calls to performPrefetch()
			UInt64 pc_hits;                // Table entry PC == eip
			UInt64 pc_misses;              // PC mismatch, new entry allocated
			UInt64 pc_evictions;           // Overwriting a non-empty entry

			// Stride learning behavior
			UInt64 new_stride_observed;    // Times stride was first set from -1
			UInt64 stride_same;            // Times new_stride == recorded stride
			UInt64 stride_change;          // Times new_stride != recorded stride (!=-1)
			UInt64 zero_stride;            // new_stride == 0
			UInt64 positive_stride;        // new_stride > 0
			UInt64 negative_stride;        // new_stride < 0

			// Threshold / training stats
			UInt64 threshold_reached;      // Times saturation_counter > prefetch_threshold
			UInt64 trained_entries;        // Entries that ever reached threshold
			UInt64 trained_but_no_prefetch;// Threshold reached but PTWTransparent failed (no PPN)

			// Extra prefetch statistics
			UInt64 extra_prefetches_issued;
			UInt64 extra_prefetches_successful;
			UInt64 extra_prefetches_failed;

			// Stride magnitude & saturation stats
			UInt64 sum_abs_stride;         // Sum of |stride| at prediction time
			UInt64 prefetch_distance_sum;  // Sum of |VPN_prefetch - VPN_current|
			UInt64 max_saturation_counter; // Max saturation_counter seen

			// Table usage pattern
			UInt64 table_accesses;         // Number of table slots touched
		} stats;

	public:
		Core *core;
		MemoryManagerBase *memory_manager;
		ShmemPerfModel *shmem_perf_model;
		int table_size;
		int prefetch_threshold;
		bool extra_prefetch;
		int lookahead;
		int degree;
		entry_prefetcher *table;

		std::string log_file_name;
		std::ofstream log_file;

		ArbitraryStridePrefetcher(Core *_core, MemoryManagerBase *_memory_manager, ShmemPerfModel *_shmem_perf_model, int table_bits, int prefetch_threshold, bool extra_prefetch, int lookahead, int degree, String name);
		std::vector<query_entry> performPrefetch(IntPtr address, IntPtr eip, Core::lock_signal_t lock, bool modeled, bool count, PageTable *pt, bool instruction = false, bool tlb_hit = false, bool pq_hit = false, int page_size = 12) override;
	};
}
