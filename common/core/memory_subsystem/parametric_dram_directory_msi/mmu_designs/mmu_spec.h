#pragma once // Ensures the header file is included only once in a compilation unit.

// Include necessary header files for memory management and hardware simulation.
#include "../memory_manager.h"
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "shmem_perf_model.h"
#include "pagetable.h"
#include "tlb_subsystem.h"
#include "mmu_base.h"
#include "mmu.h"
#include "spec_engine_base.h"
#include "base_filter.h"
#include "sim_log.h"
#include <fstream>

// Define a namespace for the ParametricDramDirectoryMSI memory model.
namespace ParametricDramDirectoryMSI
{
    class TLBHierarchy; // Forward declaration of TLBHierarchy class.

    // MemoryManagementUnitSpec: Specialized Memory Management Unit (MMU) class
    class MemoryManagementUnitSpec : public MemoryManagementUnitBase
    {
    private:
        SpecEngineBase *spec_engine;       // Pointer to a speculative execution engine.
        MemoryManagerBase *memory_manager;     // Pointer to the memory manager.
        TLBHierarchy *tlb_subsystem;       // Pointer to the hierarchical TLB subsystem.
        TLB *spec_tlb;                     // Pointer to a specialized TLB for speculative execution.
        MetadataTableBase *metadata_table; // Pointer to metadata table for memory management.
        MSHR *pt_walkers;                  // Miss Status Holding Register (MSHR) for tracking page table walks.
        BaseFilter *ptw_filter;          // Filter for page table walk results.
        // Log file handling
        SimLog *mmu_spec_log;          // SimLog instance for logging.

        // Timing experiment: CSV timeseries of PTW vs spec engine race
        std::ofstream m_timing_csv;
        UInt64 m_timing_csv_seq;       // Sequence number for each TLB miss event

        // Structure to store memory translation statistics.
        struct
        {
            UInt64 num_translations;                 // Total number of address translations performed.
            UInt64 page_faults;                      // Number of page faults encountered.
            UInt64 page_table_walks;                 // Number of page table walks performed.
            SubsecondTime total_walk_latency;        // Total latency incurred in page table walks.
            SubsecondTime total_translation_latency; // Total time spent in address translations.
            SubsecondTime total_spec_latency;        // Total latency caused to generate speculative translations.
            SubsecondTime total_tlb_latency;         // Total time spent in TLB lookups.
            SubsecondTime total_fault_latency;       // Total latency incurred due to page faults.
            SubsecondTime walker_is_active;          // Tracks active time for page table walker.
            SubsecondTime *tlb_latency_per_level;    // Array storing TLB lookup latency per level.
            UInt64 *tlb_hit_page_sizes;              // Array storing the size of pages hit in the TLB.
            UInt64 spec_late_mispredict;              // Misspeculated prefetches that arrived after PTW resolved.
            UInt64 spec_late_mispredict_pt;           // Misspeculated PT prefetches that arrived after PTW resolved.
        } translation_stats;

    public:
        // Constructor: Initializes the MMU with required parameters.
        MemoryManagementUnitSpec(Core *core, MemoryManagerBase *memory_manager, ShmemPerfModel *shmem_perf_model, String name, MemoryManagementUnitBase *nested_mmu);

        // Destructor: Cleans up resources used by the MMU.
        ~MemoryManagementUnitSpec();

        // Instantiate a page table walker for handling page faults.
        void instantiatePageTableWalker();

        // Instantiate a metadata table for managing memory metadata.
        void instantiateMetadataTable();

        // Instantiate the TLB subsystem to handle address translations.
        void instantiateTLBSubsystem();

        // Register MMU-related performance statistics.
        void registerMMUStats();

        // Discover Virtual Memory Areas (VMAs) for address translation.
        void discoverVMAs();

        // Filters the result of a page table walk based on given parameters.
        PTWResult filterPTWResult(IntPtr virtual_address, PTWResult ptw_result, PageTable *page_table, bool count);

        // Performs address translation for a given instruction or data address.
        IntPtr performAddressTranslation(IntPtr eip, IntPtr address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count);

        // Returns a pointer to the page table managed by this MMU.
        PageTable *getPageTable();

        // We overrid this function. We want to have access to the physical addresses of the pt frames
        PTWOutcome performPTW(IntPtr address, bool modeled, bool count, bool is_prefetch, IntPtr eip, Core::lock_signal_t lock, PageTable *page_table, bool restart_walk, bool instruction = false) override;

        // Returns the speculative engine associated with this MMU.
        SpecEngineBase* getSpecEngine() { return spec_engine; }
    };
}
