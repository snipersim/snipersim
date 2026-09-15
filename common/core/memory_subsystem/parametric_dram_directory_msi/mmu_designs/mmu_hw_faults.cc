/**
 * @file mmu_hw_faults.cc
 * @brief Hardware Fault Handler MMU - Delegated Memory Page Allocation
 *
 * This MMU implements a hardware-based page fault handler using a reserved
 * pool of physical memory (delegated memory). When a page fault occurs,
 * the hardware fault handler attempts to allocate a page from this pool
 * before falling back to the OS exception handler.
 *
 * ============================================================================
 * HARDWARE FAULT HANDLER ARCHITECTURE
 * ============================================================================
 *
 * Traditional Page Fault Flow:
 *   TLB Miss → PTW → Not Present → #PF Trap → Kernel → Allocate → Return
 *
 * HW Fault Handler Flow:
 *   TLB Miss → PTW → Not Present → HW Fault Handler → Allocate from Pool →
 *   Continue (NO TRAP if pool has pages!)
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │                  HW FAULT HANDLER TRANSLATION FLOW                       │
 * ├─────────────────────────────────────────────────────────────────────────┤
 * │                                                                          │
 * │   Virtual Addr ──► TLB Lookup ──┬──► HIT ──► Physical Address            │
 * │                                 │                                        │
 * │                                 └──► MISS                                │
 * │                                       │                                  │
 * │                                       ▼                                  │
 * │                               Page Table Walk                            │
 * │                                       │                                  │
 * │                              +--------+--------+                         │
 * │                              ▼                 ▼                         │
 * │                        PAGE PRESENT      NOT PRESENT                     │
 * │                              │                 │                         │
 * │                              │                 ▼                         │
 * │                              │    ┌───────────────────────┐              │
 * │                              │    │  HW Fault Handler     │              │
 * │                              │    │  (Delegated Memory)   │              │
 * │                              │    └───────────┬───────────┘              │
 * │                              │                │                          │
 * │                              │         +------+------+                   │
 * │                              │         ▼             ▼                   │
 * │                              │    POOL HAS       POOL EMPTY              │
 * │                              │      PAGE         (Real Fault)            │
 * │                              │         │             │                   │
 * │                              │         │             ▼                   │
 * │                              │         │    Kernel Exception             │
 * │                              │         │         Handler                 │
 * │                              +---------+---------+                       │
 * │                                        │                                 │
 * │                                        ▼                                 │
 * │                               Physical Address                           │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * DELEGATED MEMORY POOL:
 * ----------------------
 * The HW Fault Handler manages a contiguous region of physical memory:
 * - delegated_memory_start: Start address of the pool
 * - delegated_memory_size: Size of the pool (in bytes)
 * - page_granularity: Page size for allocations (typically 4KB)
 *
 * A tag array tracks which pages are allocated vs free.
 *
 * KEY BENEFITS:
 * - Avoids kernel trap overhead for page faults
 * - Reduces latency for demand paging
 * - Useful for memory-intensive workloads
 *
 * @see hw_fault_handler.h for HWFaultHandler implementation
 *
 * @author Konstantinos Kanellopoulos
 * @author SAFARI Research Group
 */

#include "mmu_hw_faults.h"
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
#include "core.h"
#include "thread.h"
#include "filter_factory.h"
#include "misc/exception_handler_base.h"
// sniper_space_exception_handler.h no longer needed — using base-class interface
#include <iostream>
#include <fstream>
#include <algorithm>

#include "debug_config.h"

using namespace std;

namespace ParametricDramDirectoryMSI
{

// ============================================================================
// Construction / Destruction
// ============================================================================

    /**
     * @brief Construct the Hardware Fault Handler MMU.
     *
     * Initializes an MMU with hardware page fault handling capability using
     * a delegated memory pool for fast page allocation.
     *
     * @param _core           Pointer to the core this MMU belongs to
     * @param _memory_manager Memory manager for cache access
     * @param _shmem_perf_model Performance model for timing
     * @param _name           Configuration name prefix
     * @param _nested_mmu     Optional nested MMU for virtualization
     */
	MemoryManagementUnitHWFault::MemoryManagementUnitHWFault(Core *_core, MemoryManagerBase *_memory_manager, ShmemPerfModel *_shmem_perf_model, String _name, MemoryManagementUnitBase *_nested_mmu)
	: MemoryManagementUnitBase(_core, _memory_manager, _shmem_perf_model, _name, _nested_mmu), memory_manager(_memory_manager)
	{
		std::cout << std::endl;
		std::cout << "[MMU] Initializing MMU for core " << core->getId() << std::endl;
		// mmu_N.log is the log file for the MMU of core N
		log_file = std::ofstream();
		log_file_name = "mmu.log." + std::to_string(core->getId());
		log_file_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + log_file_name;
		log_file.open(log_file_name.c_str());

		instantiatePageTableWalker(); // This instantiates the page table walker
		instantiateTLBSubsystem(); // This instantiates the TLB hierarchy
        instantiateHWFaultHandler(); // This instantiates the Hardware Fault Handler
		registerMMUStats(); // This instantiates the MMU stats
		std::cout << std::endl;

	}

    /**
     * @brief Destroy the Hardware Fault Handler MMU.
     *
     * Cleans up all allocated resources including TLBs and PTW structures.
     */
	MemoryManagementUnitHWFault::~MemoryManagementUnitHWFault()
	{
		log_file.close();
		delete tlb_subsystem;
		delete pt_walkers;
		delete[] translation_stats.tlb_latency_per_level;
		delete[] translation_stats.tlb_hit_page_sizes;
	}

// ============================================================================
// Initialization Methods
// ============================================================================

    /**
     * @brief Initialize the optional metadata table.
     *
     * The metadata table stores additional information about memory regions
     * such as protection bits and semantic information.
     */
	void MemoryManagementUnitHWFault::instantiateMetadataTable()
	{
		String metadata_table_name = Sim()->getCfg()->getString("perf_model/"+name+"/metadata_table_name");
		metadata_table = MetadataFactory::createMetadataTable(metadata_table_name, core, shmem_perf_model, this, memory_manager);
	}

    /**
     * @brief Initialize the Hardware Fault Handler with delegated memory.
     *
     * Sets up a pool of physical pages that can be allocated in hardware
     * without trapping to the OS. The pool is managed via a tag array.
     *
     * Pool Configuration:
     * - delegated_memory_size: 4096 pages (16MB total at 4KB pages)
     * - page_granularity: 12 (4KB pages)
     * - tag_array: Tracks free/allocated pages
     */
    void MemoryManagementUnitHWFault::instantiateHWFaultHandler()
    {
        int delegated_memory_size = 40960; // In pages of 4KB (160MB pool)
        hw_fault_handler = HWFaultHandler(0, (IntPtr)delegated_memory_size * 4096, 12);

        int tag_array_entry_size = 8;
        IntPtr tag_array_size = (delegated_memory_size * tag_array_entry_size + 4095) / 4096;
        hw_fault_handler.tag_array_start_ppn = 0;
        hw_fault_handler.tag_array_end_ppn = tag_array_size - 1;

        hw_fault_latency = SubsecondTime::NS(50);
        std::cout << "[MMU HW_FAULT] Delegated pool: " << delegated_memory_size << " pages ("
                  << (delegated_memory_size * 4 / 1024) << " MB), HW fault latency: 50 ns" << std::endl;
    }

    /**
     * @brief Initialize the Page Table Walker and Page Walk Cache.
     *
     * Creates the PWC filter for caching intermediate page table entries
     * and the MSHR structure for tracking concurrent page table walks.
     */
	void MemoryManagementUnitHWFault::instantiatePageTableWalker()
	{
		String mimicos_name = Sim()->getMimicOS()->getName();
		String page_table_type = Sim()->getCfg()->getString("perf_model/"+mimicos_name+"/page_table_type");
		String page_table_name = Sim()->getCfg()->getString("perf_model/"+mimicos_name+"/page_table_name");

		String filter_type = Sim()->getCfg()->getString("perf_model/"+name+"/ptw_filter_type");
		ptw_filter = FilterPTWFactory::createFilterPTWBase(filter_type, name, core);

		pt_walkers = new MSHR(Sim()->getCfg()->getInt("perf_model/"+name+"/page_table_walkers"));
			
	}

    /**
     * @brief Initialize the TLB hierarchy.
     *
     * Creates the standard hardware TLB hierarchy (L1 iTLB, L1 dTLB, L2 TLB, etc.)
     */
	void MemoryManagementUnitHWFault::instantiateTLBSubsystem()
	{
		tlb_subsystem = new TLBHierarchy(name, core, memory_manager, shmem_perf_model);
	}

// ============================================================================
// Statistics Registration
// ============================================================================

    /**
     * @brief Register all MMU statistics with Sniper's statistics framework.
     *
     * Statistics include standard MMU metrics. HW fault handler statistics
     * can be added for tracking delegated memory usage.
     */
	void MemoryManagementUnitHWFault::registerMMUStats()
	{
		bzero(&translation_stats, sizeof(translation_stats));

		// Statistics for the whole MMU
		registerStatsMetric(name, core->getId(), "num_translations", &translation_stats.num_translations);
		registerStatsMetric(name, core->getId(), "page_faults", &translation_stats.page_faults);
		registerStatsMetric(name, core->getId(), "page_table_walks", &translation_stats.page_table_walks);
		registerStatsMetric(name, core->getId(), "total_table_walk_latency", &translation_stats.total_walk_latency);
		registerStatsMetric(name, core->getId(), "total_tlb_latency", &translation_stats.total_tlb_latency);
		registerStatsMetric(name, core->getId(), "total_translation_latency", &translation_stats.total_translation_latency);
		registerStatsMetric(name, core->getId(), "total_fault_latency", &translation_stats.total_fault_latency);
		registerStatsMetric(name, core->getId(), "total_hw_fault_latency", &translation_stats.total_hw_fault_latency);
		registerStatsMetric(name, core->getId(), "page_faults_hw_handled", &translation_stats.page_faults_hw_handled);
		registerStatsMetric(name, core->getId(), "page_faults_os_handled", &translation_stats.page_faults_os_handled);

		// This statistic can be used to compare it against the *.active counter which is exposed through performance counters in a real system
		registerStatsMetric(name, core->getId(), "walker_is_active", &translation_stats.walker_is_active);

		// Statistics for TLB subsystem
		translation_stats.tlb_latency_per_level = new SubsecondTime[tlb_subsystem->getTLBSubsystem().size()];

		// Keep track of the tlb latency for each TLB level
		for (UInt32 i = 0; i < tlb_subsystem->getTLBSubsystem().size(); i++){
			registerStatsMetric(name, core->getId(), "tlb_latency_" + itostr(i), &translation_stats.tlb_latency_per_level[i]);
		}
			
	}

// ============================================================================
// Address Translation - Core MMU Operation
// ============================================================================

	/**
	 * @brief Perform address translation with HW fault handler support.
	 *
	 * This is the main entry point for address translation. The HW Fault
	 * Handler MMU extends the standard translation flow by attempting to
	 * allocate pages from a delegated memory pool before trapping to the OS.
	 *
	 * Translation Phases:
	 * 1. TLB Hierarchy Lookup - Check all TLB levels for cached translation
	 * 2. Page Table Walk - On TLB miss, walk the radix page table
	 * 3. HW Fault Handler - If not present, try delegated memory allocation
	 * 4. Kernel Fault - Only if delegated pool is exhausted
	 * 5. TLB Allocation - Insert translation into TLB hierarchy
	 *
	 * @param eip          Instruction pointer causing this access
	 * @param address      Virtual address to translate
	 * @param instruction  True if this is an instruction fetch
	 * @param lock         Cache coherence lock signal
	 * @param modeled      True to model timing
	 * @param count        True to update statistics
	 * @return Physical address, or -1 if userspace page fault
	 */

	IntPtr MemoryManagementUnitHWFault::performAddressTranslation(IntPtr eip, IntPtr address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count)
	{

		dram_accesses_during_last_walk = 0;

#if DEBUG_MMU >= DEBUG_BASIC
		log_file << std::endl;
		log_file << "[MMU] ---- Starting address translation for virtual address: " << address <<  " ---- at time " << shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD) << std::endl;
#endif

		SubsecondTime time = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD); 

		// Cleanup so that we do not have any completed entries left inside the MSHRs of the PT walkers
		pt_walkers->removeCompletedEntries(time);

		if (count)
			translation_stats.num_translations++;

		TLBSubsystem tlbs = tlb_subsystem->getTLBSubsystem(); // Get the TLB hierarchy

		// ====================================================================
		// PHASE 1: TLB Hierarchy Lookup
		// ====================================================================

		bool hit = false; // Variables to keep track of TLB hits
		TLB *hit_tlb = NULL; // We need to keep track of the TLB that hit

		CacheBlockInfo *tlb_block_info_hit = NULL; // If there is a TLB hit, we need to keep track of the block info (which eventually contains the translation)
		CacheBlockInfo *tlb_block_info = NULL; // This is the block info that we get from the TLB lookup
		
		int hit_level = -1;
		int page_size = -1; // This variable will reflect if the virtual address is mapped to a 4KB page or a 2MB page

		IntPtr ppn_result = 0;
		// We iterate through the TLB hierarchy to find if there is a TLB hit
		for (UInt32 i = 0; i < tlbs.size(); i++)
		{
#if DEBUG_MMU >= DEBUG_BASIC
			log_file << "[MMU] Searching TLB at level: " << i << std::endl;
#endif
			for (UInt32 j = 0; j < tlbs[i].size(); j++)
			{
				bool tlb_stores_instructions = (tlbs[i][j]->getType() == TLBtype::Instruction) || (tlbs[i][j]->getType() == TLBtype::Unified);

				// If the TLB stores instructions, we need to check if the address is an instruction address
				if (tlb_stores_instructions && instruction)
				{
					// @kanellok: Passing the page table to the TLB lookup function is a legacy from the old TLB implementation. 
					// It is not used in the current implementation.

					tlb_block_info = tlbs[i][j]->lookup(address, time, count, lock, eip, modeled, count, NULL);

					if (tlb_block_info != NULL) // If we have a hit in TLB
					{
						tlb_block_info_hit = tlb_block_info;
						hit_tlb = tlbs[i][j]; // Keep track of the TLB that hit
						hit_level = i; // Keep track of the level of the TLB that hit
						hit = true; // We have a hit
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
			if (hit) // We search every TLB in the level until we find a hit but we break if we find a hit in the level 
			{
#if DEBUG_MMU >= DEBUG_BASIC
			log_file << "[MMU] TLB Hit at level: " << hit_level << " at TLB " << hit_tlb->getName() << std::endl;
#endif
				break;
			}
		}


		SubsecondTime charged_tlb_latency = SubsecondTime::Zero();
		
		//If we have a TLB hit, we need to charge the TLB hit latency
		if (hit)
		{
			ppn_result = tlb_block_info_hit->getPPN(); // We get the PPN from the block info
			page_size = tlb_block_info_hit->getPageSize(); // We get the page size from the block info
			#if DEBUG_MMU >= DEBUG_BASIC
				log_file << "[MMU] TLB Hit ? " << hit << " at level: " << hit_level << " at TLB: " << hit_tlb->getName() << std::endl;
			#endif
			
			if (instruction)
				tlbs = tlb_subsystem->getInstructionPath(); // Get the TLB path for instructions
			else
				tlbs = tlb_subsystem->getDataPath(); // Get the TLB path for data

			SubsecondTime tlb_latency[hit_level + 1]; // We need to keep track of the latency of the TLBs at each level of the hierarchy

			// We iterate through the TLBs to find the slowest component at each level of the hierarchy until the level where we had a hit
			for (int i = 0; i < hit_level; i++) 
			{
				for (UInt32 j = 0; j < tlbs[i].size(); j++) 
				{
					tlb_latency[i] = max(tlbs[i][j]->getLatency(), tlb_latency[i]);
				}
#if DEBUG_MMU >= DEBUG_BASIC
				log_file << "[MMU] Charging TLB Latency: " << tlb_latency[i] << " at level: " << i << std::endl;
#endif
				translation_stats.total_tlb_latency += tlb_latency[i];
				translation_stats.tlb_latency_per_level[i] += tlb_latency[i];
				charged_tlb_latency += tlb_latency[i];
			}

			for (UInt32 j = 0; j < tlbs[hit_level].size(); j++) // We iterate through the TLBs in the level where we had a hit
			{ 
				if (tlbs[hit_level][j] == hit_tlb) // We find the TLB that hit
				{
					translation_stats.total_tlb_latency += hit_tlb->getLatency(); // We charge the latency of the TLB that hit
					charged_tlb_latency += hit_tlb->getLatency();
					translation_stats.tlb_latency_per_level[hit_level] += hit_tlb->getLatency();

#if DEBUG_MMU >= DEBUG_BASIC
					log_file << "[MMU] Charging TLB Hit Latency: " << hit_tlb->getLatency() << " at level: " << hit_level << std::endl;
#endif
				}
			}

			// Progress the clock to the time after the TLB latency
			// This is done so that the PTW starts after the TLB latency
			// @kanellok: Be very careful if you want to play around with the timing model
			shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time + charged_tlb_latency); 
			#if DEBUG_MMU >= DEBUG_BASIC
				log_file << "[MMU] New time after charging TLB latency: " << shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD) << std::endl;
			#endif

		}

		// If we have a TLB miss, we need to charge the TLB latency based on the slowest component 
		// at each level of the hierarchy
		
		SubsecondTime tlb_latency[tlbs.size()];


		// If we have a TLB miss, we need to charge the TLB latency based on the slowest component	
		// at each level of the hierarchy, across all levels of the hierarchy

		if (!hit)
		{
			
#if DEBUG_MMU >= DEBUG_BASIC
			log_file << "[MMU] TLB Miss" << std::endl;
#endif
			for (UInt32 i = 0; i < tlbs.size(); i++) 
			{
				for (UInt32 j = 0; j < tlbs[i].size(); j++)
				{
					tlb_latency[i] = max(tlbs[i][j]->getLatency(), tlb_latency[i]);
				}
#if DEBUG_MMU >= DEBUG_BASIC
				log_file << "[MMU] Charging TLB Latency: " << tlb_latency[i] << " at level: " << i << std::endl;
#endif
				translation_stats.total_tlb_latency += tlb_latency[i];
				charged_tlb_latency += tlb_latency[i];
			}
			
			// We progress the time by the charged TLB latency so that the PTW starts after the TLB latency
			shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time + charged_tlb_latency);
			#if DEBUG_MMU >= DEBUG_BASIC
				log_file << "[MMU] New time after charging TLB latency: " << shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD) << std::endl;
			#endif
		}

		// We need to keep track of the total walk latency and the total fault latency (if there was a fault)
		SubsecondTime total_walk_latency = SubsecondTime::Zero();
		SubsecondTime total_fault_latency = SubsecondTime::Zero();

		// ====================================================================
		// PHASE 2: Page Table Walk (on TLB miss)
		// ====================================================================
		// Perform radix page table walk. If page not present, try HW fault handler.

		// We only trigger the PTW if there was a TLB miss
		if (!hit)
		{	
			// Keep track of the time before the PTW starts
			SubsecondTime time_for_pt = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

			// We will occupy an entry in the MSHRs for the PT walker
			struct MSHREntry pt_walker_entry; 
			pt_walker_entry.request_time = time_for_pt;

		
			// The system has N walkers that can be used to perform page table walks in parallel
			// We need to find if there is any delay because of all the walkers being busy
			SubsecondTime delay = pt_walkers->getSlotAllocationDelay(pt_walker_entry.request_time);	

			// We switch the time to the time when the PT walker is allocated so that we start the PTW at that time
			shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time_for_pt + delay);
			#if DEBUG_MMU >= DEBUG_BASIC
				log_file << "[MMU] New time after charging the PT walker allocation delay: " << shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD) << std::endl;
			#endif

			// returns PTW latency, PF latency, Physical Address, Page Size as a tuple
			int app_id = core->getThread()->getAppId();
			PageTable* page_table = Sim()->getMimicOS()->getPageTable(app_id);

			bool userspace_mimicos_enabled = Sim()->getMimicOS()->isUserspaceMimicosEnabled();
			
			// @kanellok: restart_walk_after_fault is now false by default
			// The MMU handles page faults and restarts the walk itself
			bool restart_walk_after_fault = false;
			bool caused_page_fault = false;
			bool had_page_fault = false;  // Persists across loop iterations for fault latency tracking
			
			// Declare ptw_result outside the loop so it's accessible after
			PTWOutcome ptw_result;

			// Loop: perform PTW, handle fault if needed, always retry.
			// HW pool only determines fault latency (50ns vs full OS trap).
			bool hw_handled_this_fault = false;
			do {
				ptw_result = performPTW(address, modeled, count, false, eip, lock, page_table, restart_walk_after_fault, instruction);
				total_walk_latency = ptw_result.latency;
				caused_page_fault = ptw_result.page_fault;

				if (count) {
					translation_stats.total_walk_latency += total_walk_latency;
					translation_stats.page_table_walks++;
				}

				if (caused_page_fault) {
					had_page_fault = true;
					translation_stats.page_faults++;
					hw_handled_this_fault = false;

					bool hw_can_handle = hw_fault_handler.canHandleFault(address);

					if (!userspace_mimicos_enabled) {
						ExceptionHandlerBase *handler = Sim()->getCoreManager()->getCoreFromID(core->getId())->getExceptionHandler();
						ExceptionHandlerBase::FaultCtx fault_ctx{};
						fault_ctx.vpn = address >> 12;
						fault_ctx.page_table = page_table;
						fault_ctx.alloc_in.metadata_frames = ptw_result.requested_frames;
						handler->handle_page_fault(fault_ctx);

						if (hw_can_handle) {
							hw_fault_handler.recordMapping(address);
							hw_handled_this_fault = true;
							translation_stats.page_faults_hw_handled++;
						} else {
							translation_stats.page_faults_os_handled++;
						}
					}
				}
			} while (caused_page_fault && !userspace_mimicos_enabled);
			
			// In case that MimicOS is enabled in userspace mode, we need to handle the page fault at at the MimicOS level
			if(caused_page_fault && userspace_mimicos_enabled)
			{

#if DEBUG_MMU >= DEBUG_BASIC
				log_file << "[MMU] Page Fault caused by address: " << address << " at time " << shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD).getNS() << std::endl;
				log_file << "[MMU] Page Fault caused in kernel-space (Sniper) by address: " << address << " at time " << shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD).getNS() << std::endl;
				log_file << "[MMU] Will trigger a context switch to VirtuOS" << std::endl;
#endif
				return static_cast<IntPtr>(-1); // If there was a page fault, we return -1 to indicate that the translation failed
			}

			/*
			We need to set the completion time:
			1) Time before PTW starts
			2) Delay because of all the walkers being busy
			3) Total walk latency
			4) Total fault latency
			*/

			// Charge fault latency: HW-handled -> 50ns, OS-handled -> full OS latency
			if (had_page_fault && !userspace_mimicos_enabled)
			{
				if (hw_handled_this_fault) {
					total_fault_latency = hw_fault_latency;
					if (count)
						translation_stats.total_hw_fault_latency += total_fault_latency;
				} else {
					total_fault_latency = Sim()->getMimicOS()->getPageFaultLatency();
				}
				if (count)
					translation_stats.total_fault_latency += total_fault_latency;
			}

			pt_walker_entry.completion_time = time_for_pt + delay + total_walk_latency + total_fault_latency;
			pt_walkers->allocate(pt_walker_entry);

			if (had_page_fault && !userspace_mimicos_enabled) {
				PseudoInstruction *i = new PageFaultRoutineInstruction(total_fault_latency);
				getCore()->getPerformanceModel()->queuePseudoInstruction(i);
			}
			shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, pt_walker_entry.completion_time);

			ppn_result = ptw_result.ppn;
			page_size = ptw_result.page_size;

			#if DEBUG_MMU >= DEBUG_BASIC
				log_file << "[MMU] New time after charging the PT walker completion time: " << shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD) << std::endl;
			#endif

		}


#if DEBUG_MMU >= DEBUG_BASIC
		log_file << "[MMU] Total Walk Latency: " << total_walk_latency << std::endl;
		log_file << "[MMU] Total Fault Latency: " << total_fault_latency << std::endl;
#endif

		// ====================================================================
		// PHASE 4: TLB Allocation (on TLB miss)
		// ====================================================================
		// After PTW or HW allocation, insert translation into TLB hierarchy.
		// Evicted entries propagate to higher TLB levels (inclusive policy).

		// instruction path: follows the instruction TLB path
		// data path: follows the data TLB path
		if (instruction)
			tlbs = tlb_subsystem->getInstructionPath();
		else
			tlbs = tlb_subsystem->getDataPath();

		std::map<int, vector<EvictedTranslation>> evicted_translations;

		// We need to allocate the entry in every "allocate on miss" TLB

		int tlb_levels = tlbs.size();

		if (tlb_subsystem->isPrefetchEnabled())
		{
			tlb_levels = tlbs.size() - 1;
#if DEBUG_MMU >= DEBUG_BASIC
			log_file << "[MMU] Prefetching is enabled" << std::endl;
#endif
		}

		for (int i = 0; i < tlb_levels; i++)
		{
			// We will check where we need to allocate the page

			for (UInt32 j = 0; j < tlbs[i].size(); j++)
			{
				// We need to check if there are any evicted translations from the previous level and allocate them
				if ((i > 0) && (evicted_translations[i - 1].size() != 0))
				{
					TLBAllocResult result;

#if DEBUG_MMU >= DEBUG_BASIC
					log_file << "[MMU] There are evicted translations from level: " << i - 1 << std::endl;
#endif
					// iterate through the evicted translations and allocate them in the current TLB
					for (UInt32 k = 0; k < evicted_translations[i - 1].size(); k++)
					{
						const EvictedTranslation& evicted = evicted_translations[i - 1][k];
#if DEBUG_MMU >= DEBUG_BASIC
						log_file << "[MMU] Evicted Translation: " << evicted.address << std::endl;
#endif
						// We need to check if the TLB supports the page size of the evicted translation
						IntPtr evicted_address = evicted.address;
						int evicted_page_size = evicted.page_size;
						IntPtr evicted_ppn = evicted.ppn;
						if (tlbs[i][j]->supportsPageSize(evicted_page_size))
						{
#if DEBUG_MMU >= DEBUG_BASIC
							log_file << "[MMU] Allocating evicted entry in TLB: Level = " << i << " Index =  " << j << std::endl;
#endif

							result = tlbs[i][j]->allocate(evicted_address, time, count, lock, evicted_page_size, evicted_ppn);

							// If the allocation was successful and we have an evicted translation, 
							// we need to add it to the evicted translations vector for

							if (result.evicted)
							{
								evicted_translations[i].push_back(EvictedTranslation(result.address, result.page_size, result.ppn));
							}
						}
					}
				}

				// We need to allocate the current translation in the TLB if:
				// 1) The TLB supports the page size of the translation
				// 2) The TLB is an "allocate on miss" TLB
				// 3) There was a TLB miss or the TLB hit was at a higher level and you need to allocate the translation in the current level
				
				if (tlbs[i][j]->supportsPageSize(page_size) && tlbs[i][j]->getAllocateOnMiss() && (!hit || (hit && hit_level > i)))
				{
					
#if DEBUG_MMU >= DEBUG_BASIC
					log_file << "[MMU] " << tlbs[i][j]->getName() << " supports page size: " << page_size << std::endl;
					log_file << "[MMU] Allocating in TLB: Level = " << i << " Index = " << j << " with page size: " << page_size << " and VPN: " << (address >> page_size) << std::endl;
#endif
					TLBAllocResult result;

					result = tlbs[i][j]->allocate(address, time, count, lock, page_size, ppn_result);
					if (result.evicted)
					{
						evicted_translations[i].push_back(EvictedTranslation(result.address, result.page_size, result.ppn));
					}
				}

			}
		}

		
		translation_stats.total_translation_latency += charged_tlb_latency + total_walk_latency + total_fault_latency;

		// ====================================================================
		// PHASE 5: Calculate Physical Address
		// ====================================================================
		// Physical address = PPN * base_page_size + offset

		IntPtr page_size_in_bytes = 1ULL << page_size;      // 2^12 = 4KB or 2^21 = 2MB
		constexpr IntPtr base_page_size_in_bytes = 4096;    // 4KB base

		IntPtr final_physical_address = (ppn_result * base_page_size_in_bytes) + (address % page_size_in_bytes);

		#if DEBUG_MMU >= DEBUG_BASIC
			log_file << "[MMU] Multiplication factor: " << page_size_in_bytes << std::endl;
			log_file << "[MMU] Offset: " << (address % page_size_in_bytes) << std::endl;
			log_file << "[MMU] Physical Address: " << final_physical_address << " PPN: " << ppn_result*base_page_size_in_bytes << " Page Size: " << page_size << std::endl;
			log_file << "[MMU] Physical Address: " << bitset<64>(final_physical_address) << " PPN:" << bitset<64>(ppn_result*base_page_size_in_bytes) << " Offset: " << bitset<64>(address % page_size_in_bytes) << std::endl;
			log_file << "[MMU] Total translation latency: " << charged_tlb_latency + total_walk_latency << std::endl;
			log_file << "[MMU] Total fault latency: " << total_fault_latency << std::endl;
			log_file << "[MMU] ---- Ending address translation for virtual address: " << address << " ----" << std::endl;
		#endif

		// We return the total translation latency and the physical address
		return final_physical_address;
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
     * @param virtual_address  Virtual address being translated
     * @param ptw_result       Raw PTW result from page table
     * @param page_table       Page table being walked
     * @param count            Whether to count statistics
     * @return Filtered PTW result with cached entries removed
     */
	PTWResult MemoryManagementUnitHWFault::filterPTWResult(IntPtr virtual_address, PTWResult ptw_result, PageTable *page_table, bool count) 
	{
		return ptw_filter->filterPTWResult(virtual_address,ptw_result, page_table, count);
	}

// ============================================================================
// VMA Management (Unused)
// ============================================================================

    /**
     * @brief Discover Virtual Memory Areas (placeholder).
     *
     * VMA discovery is not used in the current HW Fault Handler implementation.
     * The delegated memory pool operates independently of VMA tracking.
     */
	void MemoryManagementUnitHWFault::discoverVMAs()
	{
		return ;
	}

} // namespace ParametricDramDirectoryMSI
