// ============================================================================
// MMU Speculative Engine Implementation
// ============================================================================
//
// This file implements an MMU variant with a Speculative TLB Engine for the
// Sniper multi-core simulator. The speculative engine prefetches page table
// entries based on access patterns to reduce TLB miss penalties.
//
// Key Features:
// ~~~~~~~~~~~~~
//   1. Standard TLB hierarchy with TLB hit/miss handling
//   2. Page Walk Cache (PWC) for caching intermediate page table entries
//   3. Speculative Engine that predicts and prefetches translations
//   4. Sniper-space and userspace page fault handling
//
// Architecture Overview:
// ~~~~~~~~~~~~~~~~~~~~~~
//   +--------+     +-----------+     +------------+
//   |  Core  | --> | MMU Spec  | --> |   Memory   |
//   +--------+     +-----------+     +------------+
//                       |
//         +-------------+-------------+
//         |             |             |
//    +----v----+   +----v----+   +----v----+
//    |   TLB   |   |  Spec   |   |   PWC   |
//    | Subsys  |   | Engine  |   | Filter  |
//    +---------+   +---------+   +---------+
//
// Speculative Engine Behavior:
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// - On TLB miss: Invokes spec engine to predict related translations
// - After PTW: Allocates translation in spec engine for future prediction
// - Predicts intra-page-table dependencies to reduce walk latency
//
// ============================================================================

#include "mmu_spec.h"
#include "mmu_base.h"
#include "memory_manager.h"
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "pagetable_factory.h"
#include "pagetable_radix.h"
#include "metadata_factory.h"
#include "mimicos.h"
#include "instruction.h"
#include "dvfs_manager.h"
#include "core.h"
#include "thread.h"
#include "spec_engine_factory.h"
#include "filter_factory.h"
#include "misc/exception_handler_base.h"
#include "sniper_space_exception_handler.h"
#include "performance_model.h"
#include <iostream>
#include <fstream>
#include <algorithm>

#include "debug_config.h"

// ============================================================================
// Compile-Time Configuration
// ============================================================================

// NOTE: ENABLE_MMU_FILE_LOGS is no longer used - replaced with SimLog and DEBUG_MMU_SPEC

// PTW-vs-spec timing CSV now controlled by ENABLE_MMU_SPEC_CSV_LOGS in debug_config.h

using namespace std;

namespace ParametricDramDirectoryMSI
{

// ============================================================================
// Construction / Destruction
// ============================================================================

    /**
     * @brief Construct the Speculative MMU.
     * 
     * Initializes an MMU with a speculative prefetch engine that predicts
     * future translations based on observed access patterns.
     * 
     * Initialization Order:
     * 1. Create speculative engine via factory
     * 2. Instantiate Page Walk Cache (PWC) and page table walkers
     * 3. Create TLB hierarchy
     * 4. Register statistics counters
     * 
     * @param _core           Pointer to the core this MMU belongs to
     * @param _memory_manager Memory manager for cache access
     * @param _shmem_perf_model Performance model for timing
     * @param _name           Configuration name prefix
     * @param _nested_mmu     Optional nested MMU for virtualization
     */
    MemoryManagementUnitSpec::MemoryManagementUnitSpec(Core *_core, MemoryManagerBase *_memory_manager, ShmemPerfModel *_shmem_perf_model, String _name, MemoryManagementUnitBase *_nested_mmu)
        : MemoryManagementUnitBase(_core, _memory_manager, _shmem_perf_model, _name, _nested_mmu), memory_manager(_memory_manager)
    {
        std::cout << std::endl;
        std::cout << "[MMU] Initializing MMU Spec for core " << core->getId() <<  "with name: " << name << std::endl;

        // Initialize SimLog for MMU Spec (uses DEBUG_MMU_SPEC flag)
        mmu_spec_log = new SimLog("MMU_SPEC", core->getId(), DEBUG_MMU_SPEC);

        // Create the speculative TLB engine via factory pattern
        // The spec engine type is configured in the simulation config file
        spec_engine = SpecEngineFactory::createSpecEngineBase(
            Sim()->getCfg()->getString("perf_model/mmu/spec/type"), 
            core, memory_manager, shmem_perf_model, 
            Sim()->getCfg()->getString("perf_model/mmu/spec/type"));
        
        instantiatePageTableWalker();  // Creates PWC and MSHR entries
        instantiateTLBSubsystem();     // Creates TLB hierarchy from config
        registerMMUStats();            // Registers statistics with Sniper

#if ENABLE_MMU_SPEC_CSV_LOGS
        // Open CSV for timing experiment (PTW-vs-spec delta timeseries)
        std::string timing_csv_path = std::string(Sim()->getConfig()->getOutputDirectory().c_str())
            + "/" + std::string(name.c_str()) + ".ptw_vs_spec_timing.csv";
        m_timing_csv.open(timing_csv_path);
        m_timing_csv << "seq,vaddr,is_pt_spec,ptw_start_ns,ptw_end_ns,ptw_latency_ns,"
                     << "spec_completion_ns,spec_latency_ns,delta_ns,prediction_correct,had_page_fault" << std::endl;
        m_timing_csv_seq = 0;
#endif

        std::cout << std::endl;
    }

    /**
     * @brief Destroy the Speculative MMU.
     * 
     * Cleans up all allocated resources including spec engine, TLBs, and walkers.
     */
    MemoryManagementUnitSpec::~MemoryManagementUnitSpec()
    {
        if (m_timing_csv.is_open())
            m_timing_csv.close();
        delete mmu_spec_log;
        delete tlb_subsystem;
        delete pt_walkers;
        delete[] translation_stats.tlb_latency_per_level;
        delete[] translation_stats.tlb_hit_page_sizes;
    }

// ============================================================================
// Initialization Methods
// ============================================================================

    /**
     * @brief Instantiate metadata table for tagged memory support.
     * 
     * Creates a metadata table for ARM MTE-style tagged memory.
     * Currently unused but available for future extensions.
     */
    void MemoryManagementUnitSpec::instantiateMetadataTable()
    {
        String metadata_table_name = Sim()->getCfg()->getString("perf_model/" + name + "/metadata_table_name");
        metadata_table = MetadataFactory::createMetadataTable(metadata_table_name, core, shmem_perf_model, this, memory_manager);
    }

    /**
     * @brief Initialize the Page Table Walker and Page Walk Cache.
     * 
     * Creates two key components:
     * 
     * 1. Page Walk Cache (PWC/ptw_filter):
     *    - Caches intermediate page table entries (PML4, PDPT, PD)
     *    - Reduces PTW memory accesses on repeated translations
     *    - Configured via "ptw_filter_type" setting
     * 
     * 2. PTW MSHRs:
     *    - Miss Status Holding Registers for parallel page table walks
     *    - Number configured via "page_table_walkers" setting
     */
    void MemoryManagementUnitSpec::instantiatePageTableWalker()
    {
        String mimicos_name = Sim()->getMimicOS()->getName();
        String page_table_type = Sim()->getCfg()->getString("perf_model/" + mimicos_name + "/page_table_type");
        String page_table_name = Sim()->getCfg()->getString("perf_model/" + mimicos_name + "/page_table_name");

        // Create Page Walk Cache (PWC) - caches intermediate PT entries
        String filter_type = Sim()->getCfg()->getString("perf_model/" + name + "/ptw_filter_type");
        ptw_filter = FilterPTWFactory::createFilterPTWBase(filter_type, name, core);

        // Create MSHRs for parallel page table walks
        pt_walkers = new MSHR(Sim()->getCfg()->getInt("perf_model/" + name + "/page_table_walkers"));
    }

    /**
     * @brief Initialize the TLB hierarchy.
     * 
     * Creates a multi-level TLB hierarchy (L1 iTLB, L1 dTLB, L2 TLB, etc.)
     * based on configuration. The TLBHierarchy class handles the details.
     */
    void MemoryManagementUnitSpec::instantiateTLBSubsystem()
    {
        tlb_subsystem = new TLBHierarchy(name, core, memory_manager, shmem_perf_model);
    }

// ============================================================================
// Statistics Registration
// ============================================================================

    /**
     * @brief Register all MMU statistics with Sniper's statistics framework.
     * 
     * Statistics include:
     * - num_translations: Total address translations performed
     * - page_faults: Pages not present in memory
     * - total_table_walk_latency: Cumulative PTW time
     * - total_tlb_latency: Cumulative TLB lookup time
     * - total_spec_latency: Time spent in speculative engine
     * - total_translation_latency: End-to-end translation time
     * - total_fault_latency: Time handling page faults
     * - walker_is_active: Cycles with active page table walks
     * - tlb_latency_N: Per-level TLB latency breakdown
     */
    void MemoryManagementUnitSpec::registerMMUStats()
    {
        translation_stats = {};

        // Core translation statistics
        registerStatsMetric(name, core->getId(), "num_translations", &translation_stats.num_translations);
        registerStatsMetric(name, core->getId(), "page_faults", &translation_stats.page_faults);
        registerStatsMetric(name, core->getId(), "total_table_walk_latency", &translation_stats.total_walk_latency);
        registerStatsMetric(name, core->getId(), "total_tlb_latency", &translation_stats.total_tlb_latency);
        registerStatsMetric(name, core->getId(), "total_spec_latency", &translation_stats.total_spec_latency);
        registerStatsMetric(name, core->getId(), "total_translation_latency", &translation_stats.total_translation_latency);
        registerStatsMetric(name, core->getId(), "total_fault_latency", &translation_stats.total_fault_latency);

        // Walker activity (comparable to hardware perf counters)
        registerStatsMetric(name, core->getId(), "walker_is_active", &translation_stats.walker_is_active);

        // Misspeculated prefetches arriving after PTW resolved (useless late traffic)
        registerStatsMetric(name, core->getId(), "spec_late_mispredict", &translation_stats.spec_late_mispredict);
        registerStatsMetric(name, core->getId(), "spec_late_mispredict_pt", &translation_stats.spec_late_mispredict_pt);

        // Per-level TLB latency statistics
        translation_stats.tlb_latency_per_level = new SubsecondTime[tlb_subsystem->getTLBSubsystem().size()];
        for (UInt32 i = 0; i < tlb_subsystem->getTLBSubsystem().size(); i++)
        {
            registerStatsMetric(name, core->getId(), "tlb_latency_" + itostr(i), &translation_stats.tlb_latency_per_level[i]);
        }
    }

// ============================================================================
// Address Translation - Core MMU Operation
// ============================================================================

    /**
     * @brief Perform virtual-to-physical address translation with speculation.
     *
     * This is the main entry point for address translation. The speculative
     * MMU extends the base translation flow with prediction capabilities.
     *
     * ┌─────────────────────────────────────────────────────────────────────┐
     * │                SPECULATIVE TRANSLATION FLOW                          │
     * ├─────────────────────────────────────────────────────────────────────┤
     * │                                                                      │
     * │   Virtual Addr ──► TLB Lookup ──┬──► HIT ──► Physical Address        │
     * │                                 │                                    │
     * │                                 └──► MISS                            │
     * │                                       │                              │
     * │                              ┌────────┴────────┐                     │
     * │                              ▼                 ▼                     │
     * │                       Page Table Walk    Spec Engine                 │
     * │                              │           (invoke)                    │
     * │                              ▼                                       │
     * │                    ┌─────────┴─────────┐                             │
     * │                    │   PWC Filter      │ ◄─ Filters cached entries   │
     * │                    └─────────┬─────────┘                             │
     * │                              ▼                                       │
     * │                    ┌─────────┴─────────┐                             │
     * │                    │  Spec Engine      │ ◄─ Predict intra-PT deps    │
     * │                    │  (allocate)       │                             │
     * │                    └─────────┬─────────┘                             │
     * │                              ▼                                       │
     * │                       TLB Allocation                                 │
     * │                              │                                       │
     * │                              ▼                                       │
     * │                       Physical Address                               │
     * └─────────────────────────────────────────────────────────────────────┘
     *
     * @param eip          Instruction pointer causing this access
     * @param address      Virtual address to translate
     * @param instruction  True if this is an instruction fetch
     * @param lock         Cache coherence lock signal
     * @param modeled      True to model timing
     * @param count        True to update statistics
     * @return Physical address on success, -1 on page fault (userspace mode)
     */
    IntPtr MemoryManagementUnitSpec::performAddressTranslation(IntPtr eip, IntPtr address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count)
    {
        // Track DRAM accesses for power/performance analysis
        dram_accesses_during_last_walk = 0;

        mmu_spec_log->debug("");
        mmu_spec_log->debug("[MMU] ---- Starting address translation for virtual address: ", address, " ---- at time ", shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD));

        SubsecondTime time = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

        // Cleanup completed PTW entries from MSHR to free walker slots
        pt_walkers->removeCompletedEntries(time);

        if (count)
            translation_stats.num_translations++;

        // Get reference to TLB hierarchy
        TLBSubsystem tlbs = tlb_subsystem->getTLBSubsystem();

        // ====================================================================
        // PHASE 1: TLB Hierarchy Lookup
        // ====================================================================

        bool hit = false;
        TLB *hit_tlb = NULL;
        CacheBlockInfo *tlb_block_info_hit = NULL;
        CacheBlockInfo *tlb_block_info = NULL;
        int hit_level = -1;
        int page_size = -1;
        IntPtr ppn_result = 0;

        // Search through TLB levels for a cached translation
        for (UInt32 i = 0; i < tlbs.size(); i++)
        {
            mmu_spec_log->debug("[MMU] Searching TLB at level: ", i);
            for (UInt32 j = 0; j < tlbs[i].size(); j++)
            {
                bool tlb_stores_instructions = (tlbs[i][j]->getType() == TLBtype::Instruction) || (tlbs[i][j]->getType() == TLBtype::Unified);

                if (tlb_stores_instructions && instruction)
                {
                    tlb_block_info = tlbs[i][j]->lookup(address, time, count, lock, eip, modeled, count, NULL);

                    if (tlb_block_info != NULL)
                    {
                        tlb_block_info_hit = tlb_block_info;
                        hit_tlb = tlbs[i][j];
                        hit_level = i;
                        hit = true;
                    }
                }
                else if (!instruction)
                {
                    bool tlb_stores_data = !(tlbs[i][j]->getType() == TLBtype::Instruction);
                    if (tlb_stores_data)
                    {
                        tlb_block_info = tlbs[i][j]->lookup(address, time, count, lock, eip, modeled, count, NULL);
                        if (tlb_block_info != NULL)
                        {
                            tlb_block_info_hit = tlb_block_info;
                            hit_tlb = tlbs[i][j];
                            hit_level = i;
                            hit = true;
                        }
                    }
                }
            }
            
            // Found a hit - stop searching at this level
            if (hit)
            {
                mmu_spec_log->debug("[MMU] TLB Hit at level: ", hit_level, " at TLB ", hit_tlb->getName());
                break;
            }
        }

        // ====================================================================
        // PHASE 2: Charge TLB Latency
        // ====================================================================

        SubsecondTime charged_tlb_latency = SubsecondTime::Zero();

        // ----- TLB HIT: Charge latency up to the hit level -----
        if (hit)
        {
            ppn_result = tlb_block_info_hit->getPPN();
            page_size = tlb_block_info_hit->getPageSize();

            mmu_spec_log->debug("[MMU] TLB Hit ? ", hit, " at level: ", hit_level, " at TLB: ", hit_tlb->getName());

            // Select instruction or data path for latency accounting
            if (instruction)
                tlbs = tlb_subsystem->getInstructionPath();
            else
                tlbs = tlb_subsystem->getDataPath();

            SubsecondTime tlb_latency[hit_level + 1];

            // Charge latency for all levels up to (but not including) hit level
            // Use max latency at each level (parallel lookup within level)
            for (int i = 0; i < hit_level; i++)
            {
                for (UInt32 j = 0; j < tlbs[i].size(); j++)
                {
                    tlb_latency[i] = max(tlbs[i][j]->getLatency(), tlb_latency[i]);
                }
                mmu_spec_log->debug("[MMU] Charging TLB Latency: ", tlb_latency[i], " at level: ", i);
                translation_stats.total_tlb_latency += tlb_latency[i];
                translation_stats.tlb_latency_per_level[i] += tlb_latency[i];
                charged_tlb_latency += tlb_latency[i];
            }

            // Find and charge latency for the hitting TLB
            // Find and charge latency for the hitting TLB
            for (UInt32 j = 0; j < tlbs[hit_level].size(); j++)
            {
                if (tlbs[hit_level][j] == hit_tlb)
                {
                    translation_stats.total_tlb_latency += hit_tlb->getLatency();
                    charged_tlb_latency += hit_tlb->getLatency();
                    translation_stats.tlb_latency_per_level[hit_level] += hit_tlb->getLatency();

                    mmu_spec_log->debug("[MMU] Charging TLB Hit Latency: ", hit_tlb->getLatency(), " at level: ", hit_level);
                }
            }

            // Advance simulation time past TLB lookup
            // NOTE: Be careful when modifying timing model
            shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time + charged_tlb_latency);
            mmu_spec_log->debug("[MMU] New time after charging TLB latency: ", shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD));
        }

        // ----- TLB MISS: Charge full TLB hierarchy latency -----
        SubsecondTime tlb_latency[tlbs.size()];

        if (!hit)
        {
            mmu_spec_log->debug("[MMU] TLB Miss");
            // Charge max latency at each level (parallel lookup within level)
            for (UInt32 i = 0; i < tlbs.size(); i++)
            {
                for (UInt32 j = 0; j < tlbs[i].size(); j++)
                {
                    tlb_latency[i] = max(tlbs[i][j]->getLatency(), tlb_latency[i]);
                }
                mmu_spec_log->debug("[MMU] Charging TLB Latency: ", tlb_latency[i], " at level: ", i);
                translation_stats.total_tlb_latency += tlb_latency[i];
                charged_tlb_latency += tlb_latency[i];
            }

            // Advance simulation time past TLB miss latency
            shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time + charged_tlb_latency);
            mmu_spec_log->debug("[MMU] New time after charging TLB latency: ", shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD));
        }

        // ====================================================================
        // PHASE 3: Page Table Walk (on TLB miss)
        // ====================================================================

        SubsecondTime total_walk_latency = SubsecondTime::Zero();
        SubsecondTime total_fault_latency = SubsecondTime::Zero();
        [[maybe_unused]] SubsecondTime spec_engine_latency = SubsecondTime::Zero();

        if (!hit)
        {
            SubsecondTime time_for_pt = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

            // Allocate a PTW MSHR entry
            struct MSHREntry pt_walker_entry;
            pt_walker_entry.request_time = time_for_pt;

            // Check if all walkers are busy (N parallel walkers supported)
            SubsecondTime delay = pt_walkers->getSlotAllocationDelay(pt_walker_entry.request_time);

            // Advance time to when walker becomes available
            shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time_for_pt + delay);
            mmu_spec_log->debug("[MMU] New time after charging the PT walker allocation delay: ", shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD));

            bool userspace_mimicos_enabled = Sim()->getMimicOS()->isUserspaceMimicosEnabled();
            bool restart_walk_after_fault = false;  // MMU handles faults internally
            bool caused_page_fault = false;
            bool had_page_fault = false;  // Persists across loop iterations
            
            int app_id = core->getThread()->getAppId();
            PageTable *page_table = Sim()->getMimicOS()->getPageTable(app_id);
            
            PTWOutcome ptw_result;

            // PTW loop: perform walk, handle faults, retry if needed
            do {
                ptw_result = performPTW(address, modeled, count, false, eip, lock, page_table, restart_walk_after_fault, instruction);

                total_walk_latency = ptw_result.latency;
                caused_page_fault = ptw_result.page_fault;

                if (caused_page_fault) {
                    had_page_fault = true;  // Track that a fault occurred (persists after retry)
                    mmu_spec_log->trace("[MMU] Page Fault caused in kernel-space (Sniper) by address: ", address, " at time ", shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD).getNS());
                    translation_stats.page_faults++;
                    
                    // Handle page fault internally (sniper-space mode)
                    if (!userspace_mimicos_enabled)
                    {
                        mmu_spec_log->trace("[MMU] Handling page fault in sniper-space mode, calling exception handler");
                        ExceptionHandlerBase *handler = Sim()->getCoreManager()->getCoreFromID(core->getId())->getExceptionHandler();

                        ExceptionHandlerBase::FaultCtx fault_ctx{};
                        fault_ctx.vpn = address >> 12;
                        fault_ctx.page_table = page_table;
                        fault_ctx.alloc_in.metadata_frames = ptw_result.requested_frames;
                        handler->handle_page_fault(fault_ctx);
                        
                        mmu_spec_log->trace("[MMU] Page fault handled, restarting PTW for address: ", address);
                        // Loop retries PTW after fault is handled
                    }
                }
            } while (caused_page_fault && !userspace_mimicos_enabled);

            // Userspace mode: return -1 to signal fault to MimicOS
            if (caused_page_fault && userspace_mimicos_enabled)
            {
                mmu_spec_log->debug("[MMU] Page Fault caused by address: ", address, " at time ", shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD).getNS());
                mmu_spec_log->debug("[MMU] Page Fault caused in kernel-space (Sniper) by address: ", address, " at time ", shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD).getNS());
                mmu_spec_log->debug("[MMU] Will trigger a context switch to MimicOS");
                return static_cast<IntPtr>(-1);
            }

            // Calculate MSHR completion time:
            // start_time + walker_delay + walk_latency + fault_latency
            
            // Track walk latency stat (total_walk_latency holds the final successful walk)
            if (count)
                translation_stats.total_walk_latency += total_walk_latency;

            // Charge page fault handling latency if a fault occurred during the loop
            if (had_page_fault && !userspace_mimicos_enabled)
            {
                total_fault_latency = Sim()->getMimicOS()->getPageFaultLatency();
                if (count)
                {
                    translation_stats.total_fault_latency += total_fault_latency;
                }
            }
            
            pt_walker_entry.completion_time = time_for_pt + delay + total_walk_latency + total_fault_latency;
            pt_walkers->allocate(pt_walker_entry);

            // Update ppn_result and page_size from PTW outcome BEFORE using them
            ppn_result = ptw_result.ppn;
            page_size = ptw_result.page_size;

            // Track walker activity (total walk time across all translations)
            if (count)
                translation_stats.walker_is_active += total_walk_latency + total_fault_latency;

            // Invoke speculative engine after successful PTW
            IntPtr physical_addr = (ppn_result << 12) | (address & ((1 << page_size) - 1));

            mmu_spec_log->debug("[MMU] Virtual Address: ", address);
            mmu_spec_log->debug("[MMU] Physical Address: ", physical_addr);
            mmu_spec_log->debug("[MMU] Page Size: ", page_size);
            mmu_spec_log->debug("[MMU] Caused Page Fault? - ", (had_page_fault ? "Yes" : "No"));
            if (!had_page_fault)
                spec_engine->invokeSpecEngine(address, count, lock, eip, modeled, time_for_pt, physical_addr);

            // ---- Track misspeculated prefetches arriving after PTW resolved ----
            if (!had_page_fault && count)
            {
                SubsecondTime ptw_end_stat = time_for_pt + delay + total_walk_latency;
                SubsecondTime spec_end_stat = spec_engine->getLastSpecCompletionTime();
                bool prediction_wrong = !spec_engine->wasLastPredictionCorrect();
                // Count wrong predictions whose prefetch arrived AFTER the PTW completed
                if (prediction_wrong && spec_end_stat != SubsecondTime::Zero() && spec_end_stat > ptw_end_stat)
                    translation_stats.spec_late_mispredict++;
            }

            // ---- Timing experiment: log data-prediction PTW-vs-spec delta ----
#if ENABLE_MMU_SPEC_CSV_LOGS
            if (!had_page_fault && m_timing_csv.is_open())
            {
                SubsecondTime ptw_start = time_for_pt + delay;
                SubsecondTime ptw_end   = ptw_start + total_walk_latency;
                SubsecondTime spec_end  = spec_engine->getLastSpecCompletionTime();
                // delta > 0 means spec arrived AFTER PTW (spec lost)
                // delta < 0 means spec arrived BEFORE PTW (spec won the race)
                int64_t delta_ns = (spec_end != SubsecondTime::Zero())
                    ? (int64_t)spec_end.getNS() - (int64_t)ptw_end.getNS()
                    : 0;
                int64_t spec_latency_ns = (spec_end != SubsecondTime::Zero())
                    ? (int64_t)spec_end.getNS() - (int64_t)time_for_pt.getNS()
                    : 0;

                m_timing_csv << m_timing_csv_seq++ << ","
                             << address << ","
                             << 0 << ","  // is_pt_spec = false (data prediction)
                             << ptw_start.getNS() << ","
                             << ptw_end.getNS() << ","
                             << total_walk_latency.getNS() << ","
                             << ((spec_end != SubsecondTime::Zero()) ? std::to_string(spec_end.getNS()) : "NA") << ","
                             << spec_latency_ns << ","
                             << delta_ns << ","
                             << (spec_engine->wasLastPredictionCorrect() ? 1 : 0) << ","
                             << 0 << std::endl;
            }
#endif

            /* We need to set the time to the time after the PTW is completed.
            This is done so that the memory manager sends the request to the cache hierarchy after the PTW is completed
            */
            if (had_page_fault && !userspace_mimicos_enabled)
            {
                PseudoInstruction *i = new PageFaultRoutineInstruction(total_fault_latency);
                getCore()->getPerformanceModel()->queuePseudoInstruction(i);
                shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, pt_walker_entry.completion_time);
            }
            else
                shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, pt_walker_entry.completion_time);

            mmu_spec_log->debug("[MMU] New time after charging the PT walker completion time: ", shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD));
        }


        mmu_spec_log->debug("[MMU] Total Walk Latency: ", total_walk_latency);
        mmu_spec_log->debug("[MMU] Total Fault Latency: ", total_fault_latency);

        // ====================================================================
        // PHASE 4: TLB Allocation (on miss or lower level hit)
        // ====================================================================

        // Select instruction or data path for allocation
        if (instruction)
            tlbs = tlb_subsystem->getInstructionPath();
        else
            tlbs = tlb_subsystem->getDataPath();

        // Track evicted translations for allocation at lower levels
        std::map<int, vector<EvictedTranslation>> evicted_translations;

        int tlb_levels = tlbs.size();

        // Skip last level if prefetching is enabled (reserved for prefetch entries)
        if (tlb_subsystem->isPrefetchEnabled())
        {
            tlb_levels = tlbs.size() - 1;
            mmu_spec_log->debug("[MMU] Prefetching is enabled");
        }

        // Iterate through TLB levels for allocation
        for (int i = 0; i < tlb_levels; i++)
        {
            for (UInt32 j = 0; j < tlbs[i].size(); j++)
            {
                // Handle evicted translations from upper level
                if ((i > 0) && (!evicted_translations[i - 1].empty()))
                {
                    TLBAllocResult result;

                    mmu_spec_log->debug("[MMU] There are evicted translations from level: ", i - 1);
                    // Allocate evicted translations in current level
                    for (UInt32 k = 0; k < evicted_translations[i - 1].size(); k++)
                    {
                        const EvictedTranslation& evicted = evicted_translations[i - 1][k];
                        mmu_spec_log->debug("[MMU] Evicted Translation: ", evicted.address);
                        IntPtr evicted_address = evicted.address;
                        int evicted_page_size = evicted.page_size;
                        IntPtr evicted_ppn = evicted.ppn;
                        
                        if (tlbs[i][j]->supportsPageSize(page_size))
                        {
                            mmu_spec_log->debug("[MMU] Allocating evicted entry in TLB: Level = ", i, " Index =  ", j);
                            result = tlbs[i][j]->allocate(evicted_address, time, count, lock, evicted_page_size, evicted_ppn);

                            // Propagate eviction to next level if needed
                            if (result.evicted)
                            {
                                evicted_translations[i].emplace_back(result.address, result.page_size, result.ppn);
                            }
                        }
                    }
                }

                // Allocate current translation if:
                // 1. TLB supports this page size
                // 2. TLB has allocate-on-miss policy
                // 3. TLB miss OR hit was at a lower level (need to populate upper levels)
                if (tlbs[i][j]->supportsPageSize(page_size) && 
                    tlbs[i][j]->getAllocateOnMiss() && 
                    (!hit || hit_level > i))
                {
                    mmu_spec_log->debug("[MMU] ", tlbs[i][j]->getName(), " supports page size: ", page_size);
                    mmu_spec_log->debug("[MMU] Allocating in TLB: Level = ", i, " Index = ", j, " with page size: ", page_size, " and VPN: ", (address >> page_size));
                    TLBAllocResult result;

                    result = tlbs[i][j]->allocate(address, time, count, lock, page_size, ppn_result);
                    if (result.evicted)
                    {
                        evicted_translations[i].emplace_back(result.address, result.page_size, result.ppn);
                    }
                }
            }
        }

        // ====================================================================
        // PHASE 5: Speculative Engine Update (on TLB miss)
        // ====================================================================
        
        // Allocate in spec engine for future prediction
        if (!hit)
        {
            spec_engine->allocateInSpecEngine(address, ppn_result, count, lock, eip, modeled);
        }

        translation_stats.total_translation_latency += charged_tlb_latency + total_walk_latency;

        // ====================================================================
        // PHASE 6: Calculate Physical Address
        // ====================================================================
        
        // Physical address = PPN * base_page_size + offset
        // PPN is always at 4KB granularity
        // Offset is extracted based on actual page size (4KB or 2MB)
        
        const int page_size_in_bytes = 1 << page_size;           // 2^page_size (optimized from pow())
        constexpr int base_page_size_in_bytes = 1 << 12;         // 4KB base page

        IntPtr physical_address = (ppn_result * base_page_size_in_bytes) + (address % page_size_in_bytes);

        mmu_spec_log->debug("[MMU] Offset: ", (address % page_size_in_bytes));
        mmu_spec_log->debug("[MMU] PPN: ", ppn_result);
        mmu_spec_log->debug("[MMU] Physical Address: ", physical_address, " PPN: ", ppn_result * base_page_size_in_bytes, " Page Size: ", page_size);
        mmu_spec_log->debug("[MMU] Total translation latency: ", charged_tlb_latency + total_walk_latency);
        mmu_spec_log->debug("[MMU] Total fault latency: ", total_fault_latency);
        mmu_spec_log->debug("[MMU] ---- Ending address translation for virtual address: ", address, " ----");

        return physical_address;
    }

// ============================================================================
// Page Table Walk Support
// ============================================================================

    /**
     * @brief Filter PTW result through Page Walk Cache (PWC).
     * 
     * Removes intermediate page table entries that are already cached in the
     * PWC, reducing the effective walk latency.
     * 
     * @param address     Virtual address being translated
     * @param ptw_result  Raw PTW result from page table
     * @param page_table  Page table being walked
     * @param count       Whether to count statistics
     * @return Filtered PTW result with cached entries removed
     */
    PTWResult MemoryManagementUnitSpec::filterPTWResult(IntPtr address, PTWResult ptw_result, PageTable *page_table, bool count)
    {
        return ptw_filter->filterPTWResult(address, ptw_result, page_table, count);
    }

    /**
     * @brief Discover Virtual Memory Areas (VMAs).
     * 
     * Not used in this MMU implementation. VMAs are used by range-based
     * MMUs for coalescing translations.
     */
    void MemoryManagementUnitSpec::discoverVMAs()
    {
        return;
    }

// ============================================================================
// Page Table Walk Implementation
// ============================================================================

    /**
     * @brief Perform a Page Table Walk (PTW) with speculative prediction.
     *
     * Walks the radix page table to translate a virtual address. Includes
     * integration with the speculative engine for intra-page-table prediction.
     *
     * ┌─────────────────────────────────────────────────────────────────────┐
     * │                    PAGE TABLE WALK FLOW                              │
     * ├─────────────────────────────────────────────────────────────────────┤
     * │                                                                      │
     * │   Virtual Addr ──► initializeWalk() ──► [PML4→PDPT→PD→PT]           │
     * │                           │                                          │
     * │                           ▼                                          │
     * │                    Sort & Deduplicate                                │
     * │                           │                                          │
     * │                           ▼                                          │
     * │                    Spec Engine (intra-PT) ◄─ Predict dependencies   │
     * │                           │                                          │
     * │                           ▼                                          │
     * │                    PWC Filter ◄─ Remove cached entries              │
     * │                           │                                          │
     * │                           ▼                                          │
     * │                    calculatePTWCycles()                              │
     * │                           │                                          │
     * │                           ▼                                          │
     * │                    Return (latency, fault, PPN, page_size)          │
     * └─────────────────────────────────────────────────────────────────────┘
     *
     * @param address       Virtual address to translate
     * @param modeled       Whether to model timing
     * @param count         Whether to count statistics
     * @param is_prefetch   Whether this is a prefetch operation
     * @param eip           Instruction pointer
     * @param lock          Cache coherence lock signal
     * @param page_table    Page table to walk
     * @param restart_walk  Whether to restart walk after fault
     * @param instruction   Whether this is an instruction fetch
     * @return PTWOutcome containing walk_latency, caused_fault, PPN, page_size
     */
    PTWOutcome MemoryManagementUnitSpec::performPTW(IntPtr address, bool modeled, bool count, bool is_prefetch, IntPtr eip, Core::lock_signal_t lock, PageTable *page_table, bool restart_walk, bool instruction)
    {
        SubsecondTime time_for_pt = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

        mmu_spec_log->debug("");
        mmu_spec_log->debug("[MMU_BASE]-------------- Starting PTW for address: ", address);

        // Initialize page table walk and get visited entries
        auto ptw_result = page_table->initializeWalk(address, count, is_prefetch, restart_walk);
        
        // Get list of page table entries visited during walk
        accessedAddresses visited_pts = ptw_result.accesses;

        // Remove duplicate entries (same PT entry may be visited multiple times)
        std::sort(visited_pts.begin(), visited_pts.end());
        visited_pts.erase(std::unique(visited_pts.begin(), visited_pts.end()), visited_pts.end());

        // Get physical address at last level for spec engine
        IntPtr physical_result_last_level = visited_pts[visited_pts.size() - 1].physical_addr;

        mmu_spec_log->debug("[MMU_BASE] Physical result at the last level: ", physical_result_last_level);
        mmu_spec_log->debug("[MMU_BASE] PTW result: ", ptw_result.page_size, " ", ptw_result.accesses.size(), " ", ptw_result.ppn, " ", ptw_result.pwc_latency, " ", ptw_result.fault_happened);

        for (size_t i = 0; i < visited_pts.size(); i++)
        {
            mmu_spec_log->debug("Visited PTs: id= ", i, " level= ", visited_pts[i].table_level, " depth= ", visited_pts[i].depth, " physical address= ", visited_pts[i].physical_addr, " ", visited_pts[i].is_pte);
        }

        // Invoke spec engine for intra-page-table dependency prediction
        // This helps predict which PT entries will be needed next
        if (page_table->getType() == "radix" && !ptw_result.fault_happened)
            spec_engine->invokeSpecEngine(address, count, lock, eip, modeled, time_for_pt, physical_result_last_level, visited_pts, true);

        // Rebuild result with deduplicated visited entries (preserve requested_frames)
        ptw_result = PTWResult(ptw_result.page_size, visited_pts, ptw_result.ppn, ptw_result.pwc_latency, ptw_result.fault_happened, ptw_result.requested_frames);

        assert(ptw_result.ppn != static_cast<IntPtr>(-1)); // Ensure PPN is valid

        // Save requested_frames BEFORE filterPTWResult (filter doesn't preserve it)
        int requested_frames = ptw_result.requested_frames;

        // Filter through PWC to remove already-cached entries
        ptw_result = filterPTWResult(address, ptw_result, page_table, count);

        mmu_spec_log->debug("[MMU_BASE] We accessed ", ptw_result.accesses.size(), " addresses");
        visited_pts = ptw_result.accesses;
        for (UInt32 i = 0; i < visited_pts.size(); i++)
        {
            mmu_spec_log->debug("[MMU_BASE] Address: ", visited_pts[i].physical_addr, " Level: ", visited_pts[i].depth, " Table: ", visited_pts[i].table_level, " Correct Translation: ", visited_pts[i].is_pte);
        }

        int page_size = ptw_result.page_size;
        IntPtr ppn_result = ptw_result.ppn;
        bool is_pagefault = ptw_result.fault_happened;

        // Calculate actual memory access latency for remaining entries
        SubsecondTime ptw_cycles = calculatePTWCycles(ptw_result, count, modeled, eip, lock, address, instruction);

        // ---- Track misspeculated PT prefetches arriving after PTW resolved ----
        if (!is_pagefault && count)
        {
            SubsecondTime ptw_end_stat = time_for_pt + ptw_cycles;
            SubsecondTime spec_end_stat = spec_engine->getLastSpecCompletionTime();
            bool prediction_wrong = !spec_engine->wasLastPredictionCorrect();
            if (prediction_wrong && spec_end_stat != SubsecondTime::Zero() && spec_end_stat > ptw_end_stat)
                translation_stats.spec_late_mispredict_pt++;
        }

        // ---- Timing experiment: log intra-PT speculation PTW-vs-spec delta ----
#if ENABLE_MMU_SPEC_CSV_LOGS
        if (!is_pagefault && m_timing_csv.is_open())
        {
            SubsecondTime ptw_end  = time_for_pt + ptw_cycles;
            SubsecondTime spec_end = spec_engine->getLastSpecCompletionTime();
            int64_t delta_ns = (spec_end != SubsecondTime::Zero())
                ? (int64_t)spec_end.getNS() - (int64_t)ptw_end.getNS()
                : 0;
            int64_t spec_latency_ns = (spec_end != SubsecondTime::Zero())
                ? (int64_t)spec_end.getNS() - (int64_t)time_for_pt.getNS()
                : 0;

            m_timing_csv << m_timing_csv_seq++ << ","
                         << address << ","
                         << 1 << ","   // is_pt_spec = true (page-table prediction)
                         << time_for_pt.getNS() << ","
                         << ptw_end.getNS() << ","
                         << ptw_cycles.getNS() << ","
                         << ((spec_end != SubsecondTime::Zero()) ? std::to_string(spec_end.getNS()) : "NA") << ","
                         << spec_latency_ns << ","
                         << delta_ns << ","
                         << (spec_engine->wasLastPredictionCorrect() ? 1 : 0) << ","
                         << (is_pagefault ? 1 : 0) << std::endl;
        }
#endif

        mmu_spec_log->debug("Finished PTW for address: ", address);
        mmu_spec_log->debug("PTW latency: ", ptw_cycles);
        mmu_spec_log->debug("Physical Page Number: ", ppn_result);
        mmu_spec_log->debug("Page Size: ", page_size);
        mmu_spec_log->debug("Is Page Fault?: ", (is_pagefault ? "Yes" : "No"));
        mmu_spec_log->debug("-------------- End of PTW");

        return PTWOutcome(ptw_cycles, is_pagefault, ppn_result, page_size, requested_frames);
    }

} // namespace ParametricDramDirectoryMSI
