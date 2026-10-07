
// #pragma once
// #include "memory_manager.h"
// #include "cache_cntlr.h"
// #include "subsecond_time.h"
// #include "fixed_types.h"
// #include "core.h"
// #include "shmem_perf_model.h"
// #include "pagetable.h"
// #include "tlb_subsystem.h"
// #include "mmu_base.h"
// #include "metadata_table_base.h"

// namespace ParametricDramDirectoryMSI
// {
// 	class TLBHierarchy;

// 	class MemoryManagementUnitNested : public MemoryManagementUnitBase
// 	{

// 	private:

// 		MemoryManager *memory_manager; 
//         MemoryManagementUnitBase *host_mmu;
//         MemoryManagementUnitBase *guest_mmu;
// 		String name;

// 		struct
// 		{
// 		  SubsecondTime total_translation_latency;
// 		  SubsecondTime total_guest_translation_latency;
// 		  SubsecondTime total_host_translation_latency;

// 		} translation_stats;

// 	public:
// 		MemoryManagementUnitNested(Core *core, MemoryManager *memory_manager, ShmemPerfModel *shmem_perf_model, String name, MemoryManagementUnitBase* _nested_mmu);
// 		~MemoryManagementUnitNested();
// 		void registerMMUStats();
// 		pair<SubsecondTime, IntPtr> performAddressTranslation(IntPtr eip, IntPtr address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count);
// 		pair<bool, pair<SubsecondTime, IntPtr>> performAddressTranslationRevelator(IntPtr eip, IntPtr address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count);
// 		void discoverVMAs();
// 	};
// }