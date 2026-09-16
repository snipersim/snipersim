
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

namespace ParametricDramDirectoryMSI
{

	class H2Prefetcher : public TLBPrefetcherBase
	{

	protected:
		int length;

	public:
		Core *core;
		MemoryManagerBase *memory_manager;

		ShmemPerfModel *shmem_perf_model;

		IntPtr A_address;
		IntPtr B_address;
		IntPtr C_address;

		H2Prefetcher(Core *_core, MemoryManagerBase *_memory_manager, ShmemPerfModel *_shmem_perf_model, String name);
		std::vector<query_entry> performPrefetch(IntPtr address, IntPtr eip, Core::lock_signal_t lock, bool modeled, bool count, PageTable *pt, bool instruction = false, bool tlb_hit = false, bool pq_hit = false, int page_size = 12) override;
	};
}
