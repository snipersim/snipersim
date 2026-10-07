#pragma once
#include "../memory_manager.h"
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "shmem_perf_model.h"
#include "pagetable.h"
#include "tlb_subsystem.h"
#include "mmu.h"
#include "metadata_table_base.h"
#include "sim_log.h"

namespace ParametricDramDirectoryMSI
{
	class MemoryManagementUnitVirt : public MemoryManagementUnitBase
	{

	private:
		MemoryManagerBase *memory_manager;
		MemoryManagementUnitBase *host_mmu;
		TLBHierarchy *tlb_subsystem;
		MSHR *pt_walkers;

		// SimLog for logging
		SimLog *mmu_virt_log;



		struct
		{
			UInt64 num_translations;
			UInt64 num_page_table_walks;
			UInt64 guest_page_faults;
			UInt64 host_page_faults;
			SubsecondTime total_translation_latency;
			SubsecondTime total_walk_latency;
			SubsecondTime total_guest_walk_latency;
			SubsecondTime total_host_walk_latency;
			SubsecondTime total_tlb_latency;
			SubsecondTime *tlb_latency_per_level;

		} translation_stats;

	public:
		MemoryManagementUnitVirt(Core *core, MemoryManagerBase *memory_manager, ShmemPerfModel *shmem_perf_model, String name, MemoryManagementUnitBase *nested_mmu);
		~MemoryManagementUnitVirt();
		void instantiatePageTableWalker();
		void instantiateTLBSubsystem();
		void registerMMUStats();
		void discoverVMAs();
		PTWResult filterPTWResult(IntPtr virtual_address, PTWResult ptw_result, PageTable *page_table, bool count);

		// accessCache is not overridden: MemoryManagementUnitBase::accessCache
		// already routes every page-table access through nested_mmu (the host
		// MMU) when one is present, which is what the upstream override did.
		IntPtr performAddressTranslation(IntPtr eip, IntPtr address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count);

	};
}
