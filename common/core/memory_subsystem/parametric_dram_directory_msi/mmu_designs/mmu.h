
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
#include "metadata_table_base.h"
#include "ptmshrs.h"
#include "base_filter.h"
#include "sim_log.h"
#include <array>
#include <unordered_map>

namespace ParametricDramDirectoryMSI
{
	
	class TLBHierarchy;

	class MemoryManagementUnit : public MemoryManagementUnitBase
	{

	private:

		// Backpointer to the memory manager
		MemoryManagerBase *memory_manager;

		// The TLB hierarchy for this MMU - gets instantiated in instantiateTLBSubsystem()
		TLBHierarchy *tlb_subsystem;

		// The metadata table for this MMU - gets instantiated in instantiateMetadataTable()
		// This is useful for ARM-type architectures with tagged memory support
		MetadataTableBase *metadata_table;
		MSHR *pt_walkers; 
		BaseFilter *ptw_filter;

		// Page size prediction-related members
		bool page_size_prediction_enabled;
		SubsecondTime l2_tlb_correct_prediction_latency;
		SubsecondTime l2_tlb_misprediction_latency;


		// Centralized logging
		SimLog *mmu_log;


		// For statistics-based logs 
		std::ofstream translation_latency_log;
		std::ofstream translation_events_log;
		std::ofstream reuse_distance_log;
		std::ofstream ptw_per_page_log;
		std::ofstream use_after_eviction_log;
		std::string translation_latency_log_name;
		std::string translation_events_log_name;
		std::string reuse_distance_log_name;
		std::string ptw_per_page_log_name;
		std::string use_after_eviction_log_name;
		static const SubsecondTime PTW_BUCKET_CHEAP;
		static const SubsecondTime PTW_BUCKET_MID;
		static const SubsecondTime PTW_BUCKET_COSTLY;
		static const std::array<int, 8> LATENCY_GRANULARITY_SHIFTS;
		static const IntPtr PAGE_SHIFT = 12; // 4KB pages


		struct PageMetrics
		{
			SubsecondTime total_translation_latency = SubsecondTime::Zero();
			SubsecondTime tlb_hit_latency = SubsecondTime::Zero();
			SubsecondTime tlb_miss_latency = SubsecondTime::Zero();
			SubsecondTime ptw_latency = SubsecondTime::Zero();
			UInt64 translations = 0;
			UInt64 page_table_walks = 0;
			UInt64 last_access_index = 0;
			bool has_last_access = false;
			UInt64 ptw_latency_buckets[4] = {0, 0, 0, 0}; // cheap, mid, costly, ultra
		};

		struct GranularityMetrics
		{
			SubsecondTime total_translation_latency = SubsecondTime::Zero();
			SubsecondTime tlb_hit_latency = SubsecondTime::Zero();
			SubsecondTime tlb_miss_latency = SubsecondTime::Zero();
			SubsecondTime ptw_latency = SubsecondTime::Zero();
			UInt64 translations = 0;
			UInt64 page_table_walks = 0;
		};

		struct GranularityReuse
		{
			UInt64 last_access_index = 0;
			bool has_last_access = false;
		};

		UInt64 translation_sequence_counter = 0;
		std::unordered_map<UInt64, UInt64> reuse_distance_histogram;
		std::unordered_map<UInt64, SubsecondTime> reuse_distance_latency;
		std::unordered_map<int, std::unordered_map<UInt64, UInt64>> reuse_distance_histogram_granular;
		std::unordered_map<int, std::unordered_map<UInt64, SubsecondTime>> reuse_distance_latency_granular;
		std::unordered_map<int, std::unordered_map<IntPtr, GranularityReuse>> granularity_reuse_state;
		std::unordered_map<IntPtr, PageMetrics> page_metrics;
		std::unordered_map<int, std::unordered_map<IntPtr, GranularityMetrics>> granularity_metrics;
		std::unordered_map<IntPtr, UInt64> last_eviction_index;

		// ====================================================================
		// Translation Sanity Checks (VA-PA consistency)
		// ====================================================================
		// Maps to verify translation consistency:
		// - va_to_pa_map: VA (page-aligned) -> PA (page-aligned), ensures same VA always maps to same PA
		// - pa_to_va_map: PA (page-aligned) -> VA (page-aligned), ensures no two VAs map to same PA
		bool sanity_checks_enabled;  ///< Enable translation sanity checks
		std::unordered_map<IntPtr, IntPtr> va_to_pa_map;  ///< VA -> PA mapping for consistency check
		std::unordered_map<IntPtr, IntPtr> pa_to_va_map;  ///< PA -> VA mapping for uniqueness check
		UInt64 sanity_check_violations;  ///< Count of detected violations



		// Coarse-grained translation statistics

		struct
		{
			UInt64 num_translations;
			UInt64 page_faults;
			UInt64 page_table_walks;
			UInt64 page_size_prediction_hits;
			UInt64 page_size_prediction_misses;
			SubsecondTime total_walk_latency;
			SubsecondTime total_translation_latency;
			SubsecondTime total_tlb_latency;
			SubsecondTime total_fault_latency;
			SubsecondTime walker_is_active;
			SubsecondTime *tlb_latency_per_level;
			UInt64 *tlb_hit_page_sizes;

			// Instruction vs Data breakdown
			UInt64 num_translations_instruction;
			UInt64 num_translations_data;
			UInt64 page_faults_instruction;
			UInt64 page_faults_data;
			UInt64 page_table_walks_instruction;
			UInt64 page_table_walks_data;
			UInt64 tlb_misses_instruction;
			UInt64 tlb_misses_data;
			UInt64 tlb_hits_instruction;
			UInt64 tlb_hits_data;
			SubsecondTime total_walk_latency_instruction;
			SubsecondTime total_walk_latency_data;
			SubsecondTime total_tlb_latency_instruction;
			SubsecondTime total_tlb_latency_data;
			SubsecondTime total_translation_latency_instruction;
			SubsecondTime total_translation_latency_data;

		} translation_stats;


		
	public:
		MemoryManagementUnit(Core *core, MemoryManagerBase *memory_manager, ShmemPerfModel *shmem_perf_model, String name, MemoryManagementUnitBase *nested_mmu);
		~MemoryManagementUnit();

		// MMU initialization helpers
		void instantiatePageTableWalker();
		void instantiateMetadataTable();
		void instantiateTLBSubsystem();
		void registerMMUStats();
		void discoverVMAs();
		
		// Translation helpers
		BaseFilter *getPTWFilter() override { return ptw_filter; }
		PTWResult filterPTWResult(IntPtr virtual_address, PTWResult ptw_result, PageTable *page_table, bool count);
		IntPtr performAddressTranslation(IntPtr eip, IntPtr virtual_address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count);
		
		// Per-page translation metrics
		void updatePageMetrics(IntPtr virtual_address, SubsecondTime translation_latency, bool performed_ptw, SubsecondTime translation_start_time, UInt64 translation_index, SubsecondTime walk_latency, SubsecondTime tlb_hit_latency, SubsecondTime tlb_miss_latency);
		void dumpPerPageLogs();
		void initializePerPageLogs();

	private:
		// The per-page CSV logs are written once, from the simulation-end
		// hook: destructors do not run at the end of a simulation.
		bool m_per_page_logs_dumped = false;
		static SInt64 hook_sim_end(UInt64 user, UInt64 args)
		{
			((MemoryManagementUnit*)user)->dumpPerPageLogs();
			return 0;
		}
	};

}
