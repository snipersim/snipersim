#include "tlb_prefetcher_base.h"
#include "mmu_base.h"
#include "../../memory_manager.h"
#include "debug_config.h"

namespace ParametricDramDirectoryMSI
{
	/*
	 * Walk the page table on the prefetcher's behalf.
	 *
	 * This was a stub returning an empty entry, which made the whole MMU
	 * prefetch path unreachable: it is the only caller that passes
	 * is_prefetch=true, so no prefetch walk was ever issued, no line was ever
	 * tagged, and every prefetch counter read zero however hard the TLB was
	 * pressed.
	 *
	 * The walk must not advance the caller's clock -- a prefetch is speculative
	 * work alongside the demand path, not ahead of it -- so the elapsed time is
	 * saved and restored around it, and the latency is reported in the entry's
	 * timestamp instead.
	 */
	query_entry TLBPrefetcherBase::PTWTransparent(IntPtr address, IntPtr eip,
	                                              Core::lock_signal_t lock,
	                                              bool modeled, bool count,
	                                              PageTable *pt)
	{
		query_entry q{};

		if (pt == NULL)
		{
			q.ppn = 0;
			return q;
		}

		const SubsecondTime start_time =
			shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

		auto *mmu_base = static_cast<MemoryManagementUnitBase *>(memory_manager->getMMU());
		auto ptw_result = mmu_base->performPTW(address, modeled, count,
		                                       /*is_prefetch*/ true, eip, lock, pt,
		                                       /*restart_walk*/ true);

		shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, start_time);

		q.timestamp = start_time + ptw_result.latency;
		q.address = address;
		q.ppn = ptw_result.ppn;
		q.page_size = ptw_result.page_size;
		q.payload_bits = ptw_result.payload_bits;

#if DEBUG_TLB >= DEBUG_DETAILED
		// Virtuoso gates this on getenv("RECENCY_DEBUG"); the port puts
		// per-event tracing behind the debug switches instead.
		fprintf(stderr, "[PTWT] pf=%s addr=0x%lx walk_lat_ns=%lu ppn=0x%lx "
		                "pgfault=%d start_ns=%lu avail_ns=%lu\n",
		        m_name.c_str(), (unsigned long)address,
		        (unsigned long)ptw_result.latency.getNS(),
		        (unsigned long)ptw_result.ppn, (int)ptw_result.page_fault,
		        (unsigned long)start_time.getNS(),
		        (unsigned long)q.timestamp.getNS());
#endif
		return q;
	}
}
