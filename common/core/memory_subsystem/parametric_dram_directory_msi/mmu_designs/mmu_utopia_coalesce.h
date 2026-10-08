#pragma once
// ============================================================================
// Utopia MMU with the coalesced 2MB radix walk
// ============================================================================
//
// The coalescing Utopia allocator (utopia_hash_coalesce.h) records a 2MB
// region placed in one way of the 4KB RestSeg in a second radix way table
// keyed by the 2MB VPN (three levels).  This MMU's RestSeg walk reads that
// table first and, on a miss, the per-page table (four levels); a 2MB hit
// is a 2MB translation.  Everything else -- TLBs, UTLB, CATS, the FlexSeg
// walk, migration -- is the Utopia MMU's.  The FPA + TAR walk is not
// changed by coalescing (the tag arrays hold every page).
//
// Counters: radix_2mb_lookups/hits/misses and the 2MB walk's lines
// (radix_2mb_pwc_*, radix_2mb_{internal,leaf}_*), under the MMU's name.
// ============================================================================

#include "mmu_utopia.h"

namespace ParametricDramDirectoryMSI
{
	class MemoryManagementUnitUtopiaCoalesce : public MemoryManagementUnitUtopia
	{
	private:
		struct CoalesceStats {
			UInt64 radix_2mb_lookups;
			UInt64 radix_2mb_hits;
			UInt64 radix_2mb_misses;
			SubsecondTime radix_2mb_latency_total;
			RadixWalkCounters radix_2mb;
		};
		CoalesceStats coalesce_stats;

	public:
		MemoryManagementUnitUtopiaCoalesce(Core *core, MemoryManagerBase *memory_manager, ShmemPerfModel *shmem_perf_model, String name, MemoryManagementUnitBase *nested_mmu);
		~MemoryManagementUnitUtopiaCoalesce();

		std::tuple<int, IntPtr, SubsecondTime> RestSegWalk(IntPtr address, bool instruction, IntPtr eip, Core::lock_signal_t lock, bool modeled, bool count) override;
	};
}
