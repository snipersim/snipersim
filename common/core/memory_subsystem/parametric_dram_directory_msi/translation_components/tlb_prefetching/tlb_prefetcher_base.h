
#pragma once
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "../memory_manager.h"
#include "shmem_perf_model.h"
#include "pagetable.h"
#include "cache_block_info.h"
#include "trans_defs.h"
#include <vector>

namespace ParametricDramDirectoryMSI
{

	class TLB;  // Forward declaration for TLB residency check

	class TLBPrefetcherBase
	{

	protected:
		std::vector<TLB*> m_tlb_hierarchy;  // TLB pointers for residency check
	public:
		Core *core;
		MemoryManagerBase *memory_manager;
		ShmemPerfModel *shmem_perf_model;
		String m_name;

		string log_file_name;
		std::ofstream log_file;

		TLBPrefetcherBase(Core *_core, MemoryManagerBase *_memory_manager, ShmemPerfModel *_shmem_perf_model, String name) : core(_core),
																											memory_manager(_memory_manager),
																											shmem_perf_model(_shmem_perf_model),
																											m_name(name)
		{
			log_file = std::ofstream();
			log_file_name = "tlb_prefetcher_core_" + std::to_string(core->getId()) + "_" + m_name.c_str() + ".log";
			log_file_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + log_file_name;
			log_file.open(log_file_name.c_str());
		}
		virtual ~TLBPrefetcherBase() {}

		void setTLBHierarchy(const std::vector<TLB*>& tlbs) { m_tlb_hierarchy = tlbs; }

		// Called when a TLB evicts a victim during allocate().
		// Override in prefetchers that need victim info (e.g., recency stack).
		virtual void notifyVictim(IntPtr /*victim_address*/, int /*page_size*/, IntPtr /*ppn*/) {}

		// Defined in tlb_prefetcher_base.cc: it needs the full MMU definition,
		// which cannot be included here without a cycle.
		virtual query_entry PTWTransparent(IntPtr address, IntPtr eip, Core::lock_signal_t lock, bool modeled, bool count, PageTable *pt);
		// A prefetcher is told which page size the hit was for, so that one
		// working on the 2MB plane does not have to infer it.  Prefetchers that
		// do not care name the parameter and ignore it; the compiler names every
		// one of them if this signature changes again, because they all say
		// override.
		virtual std::vector<query_entry> performPrefetch(IntPtr address, IntPtr eip, Core::lock_signal_t lock, bool modeled, bool count, PageTable *pt, bool instruction = false, bool tlb_hit = false, bool pq_hit = false, int page_size = 12) = 0;

		// Called when a prefetched entry is installed in the TLB, so a
		// prefetcher can tell a materialised prediction from a discarded one.
		virtual void notifyInstall(IntPtr /*address*/, int /*page_size*/) {}

		// Flush any payload updates the prefetcher has been holding back, at a
		// point where writing them is safe.
		virtual void flushPendingPayloadWritebacks(PageTable * /*pt*/) {}
	};
}
