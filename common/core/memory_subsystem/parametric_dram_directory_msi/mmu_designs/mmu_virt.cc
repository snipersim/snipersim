/**
 * @file mmu_virt.cc
 * @brief Virtualization MMU - Two-Level Address Translation for VMs
 *
 * This MMU implements nested page table walks required for virtualized
 * environments where a VM guest's virtual address must be translated
 * through both guest and host page tables.
 *
 * ============================================================================
 * VIRTUALIZATION ADDRESS TRANSLATION OVERVIEW
 * ============================================================================
 *
 * In a virtualized environment, address translation requires two stages:
 * 1. Guest Virtual Address (GVA) → Guest Physical Address (GPA)
 * 2. Guest Physical Address (GPA) → Host Physical Address (HPA)
 *
 * Traditional x86-64 (no virtualization):
 *   VA → PA (4 memory accesses on TLB miss)
 *
 * With Virtualization (nested paging):
 *   GVA → GPA (4 guest PT accesses, each requiring GPA→HPA translation!)
 *   For each guest PT level: GPA → HPA (4 host PT accesses)
 *   Worst case: 4 × (1 + 4) = 20 memory accesses per translation!
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │                VIRTUALIZATION TRANSLATION FLOW                           │
 * ├─────────────────────────────────────────────────────────────────────────┤
 * │                                                                          │
 * │   Guest VA ──► TLB Lookup ──┬──► HIT ──► Host Physical Address           │
 * │                             │                                            │
 * │                             └──► MISS                                    │
 * │                                   │                                      │
 * │                                   ▼                                      │
 * │   ┌─────────────────────────────────────────────────────────────────┐    │
 * │   │              STAGE 1: Guest Page Table Walk                      │    │
 * │   │                                                                  │    │
 * │   │   GVA ──► gL4 ──► gL3 ──► gL2 ──► gL1 ──► GPA                    │    │
 * │   │                                                                  │    │
 * │   │   (Each gLx access requires nested HPA translation!)             │    │
 * │   └──────────────────────────────┬──────────────────────────────────┘    │
 * │                                  │                                       │
 * │                                  ▼                                       │
 * │   ┌─────────────────────────────────────────────────────────────────┐    │
 * │   │              STAGE 2: Host Page Table Walk                       │    │
 * │   │                                                                  │    │
 * │   │   GPA ──► hL4 ──► hL3 ──► hL2 ──► hL1 ──► HPA                    │    │
 * │   └──────────────────────────────┬──────────────────────────────────┘    │
 * │                                  │                                       │
 * │                                  ▼                                       │
 * │                        Host Physical Address                             │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * KEY COMPONENTS:
 * ---------------
 * 1. Guest Page Table (via MimicOS_VM):
 *    - Translates Guest Virtual Address to Guest Physical Address
 *    - Managed by the guest operating system
 *
 * 2. Host Page Table (via MimicOS):
 *    - Translates Guest Physical Address to Host Physical Address
 *    - Managed by the hypervisor
 *
 * 3. Nested MMU (nested_mmu):
 *    - Performs host-level translation for each guest PT access
 *
 * 4. Combined TLB:
 *    - Caches final GVA → HPA mappings to avoid repeated nested walks
 *
 * CONFIGURATION:
 * --------------
 * - Uses MimicOS_VM for guest OS page table management
 * - Uses MimicOS for host OS page table management
 * - Requires nested_mmu parameter for host translation
 *
 * @author Konstantinos Kanellopoulos
 * @author SAFARI Research Group
 */


#include "mmu.h"
#include "mmu_base.h"
#include "mmu_virt.h"
#include "memory_manager.h"
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "pagetable_factory.h"
#include "pagetable_radix.h"
#include "metadata_factory.h"
#include "mimicos.h"
#include "simulator.h"
#include "instruction.h"
#include "core.h"
#include "thread.h"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <map>
#include <vector>
#include "log.h"
#include "misc/exception_handler_base.h"
// Faults are raised through SniperExceptionHandler::initFaultCtx(), the way
// mmu.cc does it in this tree, so the handler header is needed here.
#include "sniper_space_exception_handler.h"
#include "debug_config.h"
#include "sim_log.h"

using namespace std;

namespace ParametricDramDirectoryMSI
{

// ============================================================================
// Construction / Destruction
// ============================================================================

    /**
     * @brief Construct the Virtualization MMU.
     *
     * Initializes an MMU for virtualized environments that performs
     * two-level address translation (Guest VA → Guest PA → Host PA).
     *
     * @param _core           Pointer to the core this MMU belongs to
     * @param _memory_manager Memory manager for cache access
     * @param _shmem_perf_model Performance model for timing
     * @param _name           Configuration name prefix
     * @param _nested_mmu     Host MMU for GPA→HPA translation (required!)
     */
	MemoryManagementUnitVirt::MemoryManagementUnitVirt(Core *_core, MemoryManagerBase *_memory_manager, ShmemPerfModel *_shmem_perf_model, String _name, MemoryManagementUnitBase *_nested_mmu)
	: MemoryManagementUnitBase(_core, _memory_manager, _shmem_perf_model, _name, _nested_mmu), memory_manager(_memory_manager), host_mmu(_nested_mmu)
	{
		std::cout << std::endl;
		std::cout << "[MMU] Initializing Virt MMU for core " << core->getId() << std::endl;
		mmu_virt_log = new SimLog("MMU_VIRT", core->getId(), DEBUG_MMU_VIRT);

		// The nested (host) MMU and the guest OS are what make this MMU a
		// virtualised one; without them every translation below would
		// dereference a null pointer, so refuse to start instead.
		LOG_ASSERT_ERROR(nested_mmu != nullptr,
		                 "The virt MMU needs a host MMU (perf_model/host_mmu) to nest on");
		LOG_ASSERT_ERROR(Sim()->getMimicOS_VM() != nullptr,
		                 "The virt MMU needs a guest OS: set general/virtualized_environment = true");

		instantiatePageTableWalker(); // This instantiates the page table walker
		instantiateTLBSubsystem(); // This instantiates the TLB hierarchy
		registerMMUStats(); // This instantiates the MMU stats
		std::cout << std::endl;

	}

    /**
     * @brief Destroy the Virtualization MMU.
     *
     * Cleans up all allocated resources including TLBs and PTW structures.
     */
	MemoryManagementUnitVirt::~MemoryManagementUnitVirt()
	{
		delete mmu_virt_log;
		delete tlb_subsystem;
		delete pt_walkers;
		delete[] translation_stats.tlb_latency_per_level;
	}

// ============================================================================
// Initialization Methods
// ============================================================================

    /**
     * @brief Initialize the Page Table Walker.
     *
     * Uses MimicOS_VM for guest page table configuration.
     */
	void MemoryManagementUnitVirt::instantiatePageTableWalker()
	{
		pt_walkers = new MSHR(Sim()->getCfg()->getInt("perf_model/"+name+"/page_table_walkers"));
			
	}

    /**
     * @brief Initialize the TLB hierarchy.
     *
     * Creates TLB hierarchy that caches final GVA → HPA translations
     * to avoid repeated nested page table walks.
     */
	void MemoryManagementUnitVirt::instantiateTLBSubsystem()
	{
		tlb_subsystem = new TLBHierarchy(name, core, memory_manager, shmem_perf_model);
	}

// ============================================================================
// Statistics Registration
// ============================================================================

    /**
     * @brief Register all MMU statistics with Sniper's statistics framework.
     *
     * Tracks nested page walk statistics including guest and host walks.
     */
	void MemoryManagementUnitVirt::registerMMUStats()
	{
		translation_stats = {};

		// Statistics for the whole MMU

		registerStatsMetric(name, core->getId(), "num_page_table_walks", &translation_stats.num_page_table_walks);
		registerStatsMetric(name, core->getId(), "num_translations", &translation_stats.num_translations);
		registerStatsMetric(name, core->getId(), "total_translation_latency", &translation_stats.total_translation_latency);
		registerStatsMetric(name, core->getId(), "total_walk_latency", &translation_stats.total_walk_latency);
		registerStatsMetric(name, core->getId(), "total_tlb_latency", &translation_stats.total_tlb_latency);

		// The two stages of the nested walk, so that a run can show that both
		// the guest and the host page table were walked and faulted on.
		registerStatsMetric(name, core->getId(), "guest_page_faults", &translation_stats.guest_page_faults);
		registerStatsMetric(name, core->getId(), "host_page_faults", &translation_stats.host_page_faults);
		registerStatsMetric(name, core->getId(), "total_guest_walk_latency", &translation_stats.total_guest_walk_latency);
		registerStatsMetric(name, core->getId(), "total_host_walk_latency", &translation_stats.total_host_walk_latency);

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
     * @brief Perform two-level virtualized address translation.
     *
     * This is the main entry point for virtualized address translation.
     * On TLB miss, performs nested page table walks:
     * 1. Guest PTW: GVA → GPA (using guest_page_table via MimicOS_VM)
     * 2. Host PTW: GPA → HPA (using nested_mmu to access host_page_table)
     *
     * Translation Phases:
     * 1. TLB Hierarchy Lookup - Check for cached GVA → HPA mapping
     * 2. Guest Page Table Walk - GVA → GPA translation
     * 3. Host Page Table Walk - GPA → HPA translation (via nested MMU)
     * 4. TLB Allocation - Cache final GVA → HPA mapping
     *
     * @param eip          Instruction pointer causing this access
     * @param address      Guest Virtual Address to translate
     * @param instruction  True if this is an instruction fetch
     * @param lock         Cache coherence lock signal
     * @param modeled      True to model timing
     * @param count        True to update statistics
     * @return Host Physical Address, or -1 if page fault in userspace mode
     */

	IntPtr MemoryManagementUnitVirt::performAddressTranslation(IntPtr eip, IntPtr address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count)
	{

		dram_accesses_during_last_walk = 0;

		mmu_virt_log->debug("---- Starting address translation for virtual address:", address, "---- at time", shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD));

		SubsecondTime time = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD); 

		// Cleanup so that we do not have any completed entries left inside the MSHRs of the PT walkers
		pt_walkers->removeCompletedEntries(time);

		if (count)
			translation_stats.num_translations++;

		TLBSubsystem tlbs = tlb_subsystem->getTLBSubsystem(); // Get the TLB hierarchy

		// The guest page table maps this GVA; the TLB lookup takes it for the
		// TLB prefetchers, which walk it (mmu.cc passes its page table the
		// same way).
		int app_id = core->getThread()->getAppId();
		PageTable* guest_page_table = Sim()->getMimicOS_VM()->getPageTable(app_id);
		PageTable* host_page_table = Sim()->getMimicOS()->getPageTable(app_id);

		// ====================================================================
		// PHASE 1: TLB Hierarchy Lookup (GVA → HPA direct)
		// ====================================================================
		// TLB caches final GVA→HPA mappings to avoid nested PTW overhead

		bool hit = false; // Variables to keep track of TLB hits
		TLB *hit_tlb = NULL; // We need to keep track of the TLB that hit

		CacheBlockInfo *tlb_block_info_hit = NULL; // If there is a TLB hit, we need to keep track of the block info (which eventually contains the translation)
		CacheBlockInfo *tlb_block_info = NULL; // This is the block info that we get from the TLB lookup
		
		int hit_level = -1;
		int page_size = 12; // This is the page size in bits (4KB). This variable will reflect if the virtual address is mapped to a 4KB page or a 2MB page
		
		// We iterate through the TLB hierarchy to find if there is a TLB hit
		for (UInt32 i = 0; i < tlbs.size(); i++)
		{
			mmu_virt_log->debug("Searching TLB at level:", i);
			for (UInt32 j = 0; j < tlbs[i].size(); j++)
			{
				bool tlb_stores_instructions = (tlbs[i][j]->getType() == TLBtype::Instruction) || (tlbs[i][j]->getType() == TLBtype::Unified);

				// If the TLB stores instructions, we need to check if the address is an instruction address
				if (tlb_stores_instructions && instruction)
				{
					tlb_block_info = tlbs[i][j]->lookup(address, time, count, lock, eip, modeled, count, guest_page_table, true /* instruction */);

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
						tlb_block_info = tlbs[i][j]->lookup(address, time, count, lock, eip, modeled, count, guest_page_table, false /* data */);
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
				mmu_virt_log->debug("TLB Hit at level:", hit_level, "at TLB", hit_tlb->getName());
				break;
			}
		}

		// @hsongara: Charge TLB access latencies

		SubsecondTime charged_tlb_latency = SubsecondTime::Zero();
		
		if (hit)
		{
			mmu_virt_log->debug("TLB Hit ?", hit, "at level:", hit_level, "at TLB:", hit_tlb->getName());
			
			if (instruction)
				tlbs = tlb_subsystem->getInstructionPath(); // Get the TLB path for instructions
			else
				tlbs = tlb_subsystem->getDataPath(); // Get the TLB path for data

			std::vector<SubsecondTime> tlb_latency(hit_level + 1, SubsecondTime::Zero()); // We need to keep track of the latency of the TLBs at each level of the hierarchy

			// We iterate through the TLBs to find the slowest component at each level of the hierarchy until the level where we had a hit
			for (int i = 0; i < hit_level; i++) 
			{
				for (UInt32 j = 0; j < tlbs[i].size(); j++) 
				{
					tlb_latency[i] = max(tlbs[i][j]->getLatency(), tlb_latency[i]);
				}
				mmu_virt_log->trace("Charging TLB Latency:", tlb_latency[i], "at level:", i);
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

					mmu_virt_log->trace("Charging TLB Hit Latency:", hit_tlb->getLatency(), "at level:", hit_level);
				}
			}

			// Progress the clock to the time after the TLB latency
			// This is done so that the PTW starts after the TLB latency
			// @kanellok: Be very careful if you want to play around with the timing model
			shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time + charged_tlb_latency); 
			mmu_virt_log->trace("New time after charging TLB latency:", shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD));

		}

		if (!hit)
		{
			std::vector<SubsecondTime> tlb_latency(tlbs.size(), SubsecondTime::Zero());
			mmu_virt_log->debug("TLB Miss");
			for (UInt32 i = 0; i < tlbs.size(); i++) 
			{
				for (UInt32 j = 0; j < tlbs[i].size(); j++)
				{
					tlb_latency[i] = max(tlbs[i][j]->getLatency(), tlb_latency[i]);
				}
				mmu_virt_log->trace("Charging TLB Latency:", tlb_latency[i], "at level:", i);
				translation_stats.total_tlb_latency += tlb_latency[i];
				translation_stats.tlb_latency_per_level[i] += tlb_latency[i];
				charged_tlb_latency += tlb_latency[i];
			}
			
			// We progress the time by the charged TLB latency so that the PTW starts after the TLB latency
			shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time + charged_tlb_latency);
			mmu_virt_log->trace("New time after charging TLB latency:", shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD));
		}

		// @hsongara: We perform a nested PTW for a TLB miss
		// We need to keep track of the total walk latency
		SubsecondTime total_walk_latency = SubsecondTime::Zero();
		
		// This is the physical page number that we will get from the PTW
		IntPtr ppn_result;

		// ====================================================================
		// PHASE 2: Nested Page Table Walk (on TLB miss)
		// ====================================================================
		// Perform two-level PTW: Guest PTW (GVA→GPA), then Host PTW (GPA→HPA)

		// We only trigger the PTW if there was a TLB miss
		if (!hit)
		{	
			if (count)
				translation_stats.num_page_table_walks++;

			// Keep track of the time before the PTW starts
			SubsecondTime time_for_pt = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

			// @hsongara: Get a PT walker and charge corresponding latency
			struct MSHREntry pt_walker_entry;
			pt_walker_entry.request_time = time_for_pt;

			SubsecondTime delay = pt_walkers->getSlotAllocationDelay(pt_walker_entry.request_time);

			// We switch the time to the time when the PT walker is allocated so that we start the PTW at that time
			shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time_for_pt + delay);
			mmu_virt_log->trace("New time after charging the PT walker allocation delay:", shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD));

			bool userspace_mimicos_enabled = Sim()->getMimicOS()->isUserspaceMimicosEnabled();

			// ----------------------------------------------------------------
			// PHASE 2a: Guest Page Table Walk (GVA → GPA)
			// ----------------------------------------------------------------
			// Walk guest page table (gL4 → gL1) to translate GVA to GPA.
			// If guest page fault occurs, handle it and retry until resolved.

			// @hsongara: Perform gL4 to gL1 (Guest Virtual to Guest Physical)
			// @kanellok: restart_walk_after_fault is now false by default for guest PTW
			// The MMU handles page faults and restarts the walk itself
			bool restart_walk_after_fault = false;
			bool caused_guest_page_fault = false;
			
			// Declare ptw_result outside the loop so it's accessible after
			PTWOutcome ptw_result;
			
			// Loop to handle guest page faults: perform PTW, handle fault if needed, retry
			do {
				ptw_result = performPTW(address, modeled, count, false, eip, lock, guest_page_table, restart_walk_after_fault, instruction);
				caused_guest_page_fault = ptw_result.page_fault;
				
				if (caused_guest_page_fault)
				{
					mmu_virt_log->debug("Guest Page Fault occurred for GVA:", address);
					if (count)
						translation_stats.guest_page_faults++;
					
					// Handle guest page fault at MMU level (sniper-space mode)
					if (!userspace_mimicos_enabled)
					{
						mmu_virt_log->debug("Handling guest page fault in sniper-space mode, calling exception handler");
						// The fault context names the guest page table, which is
						// how the handler knows to allocate the data frame and the
						// page-table frames from the guest OS rather than the host.
						ExceptionHandlerBase *base_handler = Sim()->getCoreManager()->getCoreFromID(core->getId())->getExceptionHandler();
						SniperExceptionHandler* sniper_handler = dynamic_cast<SniperExceptionHandler*>(base_handler);
						ExceptionHandlerBase::FaultCtx fault_ctx = sniper_handler->initFaultCtx(guest_page_table, address, core->getId(), ptw_result.requested_frames, instruction);
						sniper_handler->handle_page_fault(fault_ctx);
						
						mmu_virt_log->debug("Guest page fault handled, restarting guest PTW for address:", address);
						// Loop will retry PTW after fault is handled
					}
				}
			} while (caused_guest_page_fault && !userspace_mimicos_enabled);
			
			// In case that MimicOS is enabled in userspace mode, we need to handle the guest page fault at the MimicOS level
			if (caused_guest_page_fault && userspace_mimicos_enabled)
			{
				mmu_virt_log->debug("Guest Page Fault caused in userspace by GVA:", address);
				return static_cast<IntPtr>(-1); // If there was a guest page fault in userspace mode, return -1
			}
			
			SubsecondTime total_ptw_latency = ptw_result.latency;

			// @hsongara: Charge PTW latency
			shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time_for_pt + delay + total_ptw_latency);

			// ----------------------------------------------------------------
			// PHASE 2b: Host Page Table Walk (GPA → HPA via Nested MMU)
			// ----------------------------------------------------------------
			// Translate Guest Physical Address to Host Physical Address.
			// Uses nested_mmu to walk host page table for GPA → HPA.

			// @hsongara: Translate gPA to an hPA
			// The page table returns the PPN at 4KB granularity, with the page
			// size alongside it (see mmu.cc), so the guest page's own size
			// gives the offset to keep: a 2MB guest page keeps 21 bits.
			IntPtr gppn_result = ptw_result.ppn;
			int guest_page_size = ptw_result.page_size;
			IntPtr base_page_size_in_bytes = 1ULL << 12;           // 4KB base page
			IntPtr gpa_addr = (gppn_result * base_page_size_in_bytes) + (address & ((1ULL << guest_page_size) - 1));

			//	For the gPA to hPA translation only 2 levels are cached
			// MemoryManagementUnit* nested_mmu_cast = dynamic_cast<MemoryManagementUnit*>(nested_mmu);
			// int m_pwc_max_level = nested_mmu_cast->getPTWFilter();

			// @kanellok: Host PTW (GPA to HPA) - use do-while pattern for page fault handling
			bool caused_host_page_fault = false;
			PTWOutcome gpa_ptw_result;
			
			do {
				gpa_ptw_result = nested_mmu->performPTW(gpa_addr, modeled, count, false, eip, lock, host_page_table, restart_walk_after_fault, instruction);
				caused_host_page_fault = gpa_ptw_result.page_fault;
				
				if (caused_host_page_fault)
				{
					mmu_virt_log->debug("Host Page Fault occurred for GPA:", gpa_addr);
					if (count)
						translation_stats.host_page_faults++;
					
					// Handle host page fault at MMU level (sniper-space mode)
					if (!userspace_mimicos_enabled)
					{
						mmu_virt_log->debug("Handling host page fault in sniper-space mode, calling exception handler");
						ExceptionHandlerBase *base_handler = Sim()->getCoreManager()->getCoreFromID(core->getId())->getExceptionHandler();
						SniperExceptionHandler* sniper_handler = dynamic_cast<SniperExceptionHandler*>(base_handler);
						ExceptionHandlerBase::FaultCtx fault_ctx = sniper_handler->initFaultCtx(host_page_table, gpa_addr, core->getId(), gpa_ptw_result.requested_frames, instruction);
						sniper_handler->handle_page_fault(fault_ctx);
						
						mmu_virt_log->debug("Host page fault handled, restarting host PTW for GPA:", gpa_addr);
						// Loop will retry PTW after fault is handled
					}
				}
			} while (caused_host_page_fault && !userspace_mimicos_enabled);
			
			// In case that MimicOS is enabled in userspace mode, we need to handle the host page fault at the MimicOS level
			if (caused_host_page_fault && userspace_mimicos_enabled)
			{
				mmu_virt_log->debug("Host Page Fault caused in userspace by GPA:", gpa_addr);
				return static_cast<IntPtr>(-1); // If there was a host page fault in userspace mode, return -1
			}
			
			SubsecondTime total_gpa_ptw_latency = gpa_ptw_result.latency;

			// The TLB caches GVA -> HPA at the granularity both mappings are
			// linear over: the smaller of the guest and the host page.  The
			// stored PPN is the 4KB frame of that block's base, so that a hit
			// reconstructs the address as ppn * 4KB + offset, as below.
			int host_page_size = gpa_ptw_result.page_size;
			page_size = min(guest_page_size, host_page_size);
			IntPtr hpa_addr = (gpa_ptw_result.ppn * base_page_size_in_bytes) + (gpa_addr & ((1ULL << host_page_size) - 1));
			ppn_result = (hpa_addr - (address & ((1ULL << page_size) - 1))) >> 12;


			// @hsongara: Charge gPA PTW latency
			shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time_for_pt + delay + total_ptw_latency + total_gpa_ptw_latency);

			total_walk_latency = delay + total_ptw_latency + total_gpa_ptw_latency;
			if (count)
			{
				translation_stats.total_walk_latency += total_walk_latency;
				translation_stats.total_guest_walk_latency += total_ptw_latency;
				translation_stats.total_host_walk_latency += total_gpa_ptw_latency;
			}

			pt_walker_entry.completion_time = time_for_pt + delay + total_ptw_latency + total_gpa_ptw_latency;
			pt_walkers->allocate(pt_walker_entry);

			mmu_virt_log->trace("New time after charging the PT walker completion time:", shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD));
		}
		else
		{
			/* In this scenario, we have a TLB hit. We do not need to perform 
			a PTW and we can directly get the translation from the TLB block info */
			
			page_size = tlb_block_info_hit->getPageSize();
			ppn_result = tlb_block_info_hit->getPPN();
		}

		mmu_virt_log->debug("Total Walk Latency:", total_walk_latency);

		// ====================================================================
		// PHASE 3: TLB Allocation - Populate TLBs on Miss
		// ====================================================================
		// On TLB miss, allocate the GVA→HPA translation in "allocate on miss"
		// TLBs. Evicted entries cascade to higher-level TLBs.

		// @hsongara: TLB allocate on miss
		// instruction path: follows the instruction TLB path
		// data path: follows the data TLB path
		if (instruction)
			tlbs = tlb_subsystem->getInstructionPath();
		else
			tlbs = tlb_subsystem->getDataPath();

		std::map<int, vector<EvictedTranslation>> evicted_translations;

		// We need to allocate the entry in every "allocate on miss" TLB

		int tlb_levels = tlbs.size();

		for (int i = 0; i < tlb_levels; i++)
		{
			// We will check where we need to allocate the page

			for (UInt32 j = 0; j < tlbs[i].size(); j++)
			{
				// Prefetch queues only receive prefetched entries, never demand
				// allocations or evictions (mmu.cc skips them the same way).
				if (tlbs[i][j]->getPrefetch())
					continue;

				// We need to check if there are any evicted translations from the previous level and allocate them
				if ((i > 0) && (evicted_translations[i - 1].size() != 0))
				{
					TLBAllocResult result;

					mmu_virt_log->trace("There are evicted translations from level:", i - 1);
					// iterate through the evicted translations and allocate them in the current TLB
					for (UInt32 k = 0; k < evicted_translations[i - 1].size(); k++)
					{
						const EvictedTranslation& evicted = evicted_translations[i - 1][k];
						mmu_virt_log->trace("Evicted Translation:", evicted.address);
						// We need to check if the TLB supports the page size of the evicted translation
						IntPtr evicted_address = evicted.address;
						int evicted_page_size = evicted.page_size;
						IntPtr evicted_ppn = evicted.ppn;
						if (tlbs[i][j]->supportsPageSize(evicted_page_size))
						{
							mmu_virt_log->trace("Allocating evicted entry in TLB: Level =", i, "Index =", j);

							result = tlbs[i][j]->allocate(evicted_address, time, count, lock, evicted_page_size, evicted_ppn, false /* not self_alloc */, instruction);

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
					mmu_virt_log->trace(tlbs[i][j]->getName(), "supports page size:", page_size);
					mmu_virt_log->trace("Allocating in TLB: Level =", i, "Index =", j, "with page size:", page_size, "and VPN:", (address >> page_size));
					TLBAllocResult result;

					result = tlbs[i][j]->allocate(address, time, count, lock, page_size, ppn_result, false /* not self_alloc */, instruction);
					if (result.evicted)
					{
						evicted_translations[i].push_back(EvictedTranslation(result.address, result.page_size, result.ppn));
					}
				}

			}
		}

		// ====================================================================
		// PHASE 4: Result Calculation
		// ====================================================================
		// Finalize statistics and compute final Host Physical Address

		// @hsongara: Finalize stats
		SubsecondTime total_translation_latency = charged_tlb_latency + total_walk_latency;
		translation_stats.total_translation_latency += total_translation_latency;

		IntPtr page_size_in_bytes = 1ULL << page_size;         // Convert page size bits to bytes
		IntPtr base_page_size_in_bytes = 1ULL << 12;           // 4KB base page

		IntPtr final_physical_address = (ppn_result * base_page_size_in_bytes) + (address & (page_size_in_bytes - 1));

		// We return the total translation latency and the physical address
		return final_physical_address;
	}

// ============================================================================
// Helper Functions
// ============================================================================

    /**
     * @brief Filter PTW result (not used in virtualized MMU).
     *
     * @return Empty PTWResult (placeholder for interface compatibility)
     */
	PTWResult MemoryManagementUnitVirt::filterPTWResult(IntPtr address, PTWResult ptw_result, PageTable *page_table, bool count)
	{
		return PTWResult();
	}
	
    /**
     * @brief Discover VMAs (not used in virtualized MMU).
     *
     * VMA discovery is not required for this MMU implementation.
     */
	void MemoryManagementUnitVirt::discoverVMAs()
	{
		return ;
	}

} // namespace ParametricDramDirectoryMSI
