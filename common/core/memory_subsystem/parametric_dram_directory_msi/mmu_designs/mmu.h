
#pragma once
#include "../memory_manager.h"
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "shmem_perf_model.h"
#include "pagetable.h"
#include "tlb_subsystem.h"
#include "mmu_base.h"
#include "ptmshrs.h"

namespace ParametricDramDirectoryMSI
{
	
	class TLBHierarchy;

	class MemoryManagementUnit : public MemoryManagementUnitBase
	{

	private:
		MemoryManager *memory_manager;
		TLBHierarchy *tlb_subsystem;
		MSHR *pt_walkers; 
		PWC *pwc; // Only used for radix page tables
		bool m_pwc_enabled;
		int max_pwc_level;

		//For the log
		std::ofstream log_file;
		std::string log_file_name;

		struct
		{
			UInt64 num_translations;
			UInt64 page_faults;
			UInt64 page_table_walks;
			SubsecondTime total_walk_latency;
			SubsecondTime total_translation_latency;
			SubsecondTime total_tlb_latency;
			SubsecondTime total_fault_latency;
			SubsecondTime walker_is_active;
			SubsecondTime *tlb_latency_per_level;
			UInt64 *tlb_hit_page_sizes; 

		} translation_stats;
		
	public:
		MemoryManagementUnit(Core *core, MemoryManager *memory_manager, ShmemPerfModel *shmem_perf_model, String name);
		~MemoryManagementUnit();
		void instantiatePageTableWalker();
		void instantiateMetadataTable();
		void instantiateTLBSubsystem();
		void registerMMUStats();
		void discoverVMAs();

		PTWResult filterPTWResult(PTWResult ptw_result, PageTable *page_table, bool count);
		IntPtr performAddressTranslation(IntPtr eip, IntPtr address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count);
		PageTable* getPageTable();
		void setMaxPWCLevel(int max_pwc_level_);
		int getMaxPWCLevel();
	};

}