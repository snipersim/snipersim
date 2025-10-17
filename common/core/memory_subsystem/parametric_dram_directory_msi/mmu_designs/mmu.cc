
#include "mmu.h"
#include "mmu_base.h"
#include "memory_manager.h"
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "pagetable_factory.h"
#include "pagetable_radix.h"
#include "pagetable.h"
#include "mimicos.h"
#include "instruction.h"
#include "core.h"
#include "thread.h"
#include <iostream>
#include <fstream>
#include <algorithm>


/* If you enable this flag, you will see debug messages that will help you understand the address translation process
 * This is useful for debugging purposes 
*/

//	#define DEBUG_MMU 

using namespace std;

namespace ParametricDramDirectoryMSI
{

	MemoryManagementUnit::MemoryManagementUnit(Core *_core, MemoryManager *_memory_manager, ShmemPerfModel *_shmem_perf_model, String _name)
	: MemoryManagementUnitBase(_core, _memory_manager, _shmem_perf_model, _name), memory_manager(_memory_manager)
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
		registerMMUStats(); // This instantiates the MMU stats
		std::cout << std::endl;

	}

	MemoryManagementUnit::~MemoryManagementUnit()
	{
		log_file.close();
		delete tlb_subsystem;
		delete pt_walkers;
		delete[] translation_stats.tlb_latency_per_level;
		delete[] translation_stats.tlb_hit_page_sizes;
	}
	
	/* 
	 * The MMU might also be used to retrieve metadata information (protection bits, semantic information) about the corresponding memory address 
	 * This is done by walking a metadata table that is used to store metadata information about the memory address 
	*/

	void MemoryManagementUnit::instantiateMetadataTable()
	{
	}

	void MemoryManagementUnit::instantiatePageTableWalker()
	{
		String mimicos_name = Sim()->getMimicOS()->getName();
		String page_table_type = Sim()->getCfg()->getString("perf_model/"+mimicos_name+"/page_table_type");
		String page_table_name = Sim()->getCfg()->getString("perf_model/"+mimicos_name+"/page_table_name");

		
		if (page_table_type == "radix")
		{	
			max_pwc_level = Sim()->getCfg()->getInt("perf_model/"+name+"/pwc/levels");
			m_pwc_enabled = Sim()->getCfg()->getBool("perf_model/"+name+"/pwc/enabled");

			if (m_pwc_enabled)
			{

				std::cout << "[MMU] Page walk caches are enabled" << std::endl;

				UInt32 *entries = (UInt32 *)malloc(sizeof(UInt64) * max_pwc_level);
				UInt32 *associativities = (UInt32 *)malloc(sizeof(UInt64) * max_pwc_level);
				for (int i = 0; i < max_pwc_level; i++)
				{
					entries[i] = Sim()->getCfg()->getIntArray("perf_model/" + name + "/pwc/entries", i);
					associativities[i] = Sim()->getCfg()->getIntArray("perf_model/" + name + "/pwc/associativity", i);
				}

				ComponentLatency pwc_access_latency = ComponentLatency(core->getDvfsDomain(), Sim()->getCfg()->getInt("perf_model/" + name + "/pwc/access_penalty"));
				ComponentLatency pwc_miss_latency = ComponentLatency(core->getDvfsDomain(), Sim()->getCfg()->getInt("perf_model/" + name + "/pwc/miss_penalty"));
				pwc = new PWC("pwc", "perf_model/" + name + "/pwc", core->getId(), associativities, entries, max_pwc_level, pwc_access_latency, pwc_miss_latency, false);
			
			}
		}

		pt_walkers = new MSHR(Sim()->getCfg()->getInt("perf_model/"+name+"/page_table_walkers"));
			
	}

	void MemoryManagementUnit::instantiateTLBSubsystem()
	{
		tlb_subsystem = new TLBHierarchy(name, core, memory_manager, shmem_perf_model);
	}
	
	void MemoryManagementUnit::registerMMUStats()
	{
		bzero(&translation_stats, sizeof(translation_stats));

		// Statistics for the whole MMU
		registerStatsMetric(name, core->getId(), "num_translations", &translation_stats.num_translations);
		registerStatsMetric(name, core->getId(), "page_faults", &translation_stats.page_faults);
		registerStatsMetric(name, core->getId(), "total_table_walk_latency", &translation_stats.total_walk_latency);
		registerStatsMetric(name, core->getId(), "total_tlb_latency", &translation_stats.total_tlb_latency);
		registerStatsMetric(name, core->getId(), "total_translation_latency", &translation_stats.total_translation_latency);
		registerStatsMetric(name, core->getId(), "total_fault_latency", &translation_stats.total_fault_latency);


		// This statistic can be used to compare it against the *.active counter which is exposed through performance counters in a real system
		registerStatsMetric(name, core->getId(), "walker_is_active", &translation_stats.walker_is_active);

		// Statistics for TLB subsystem
		translation_stats.tlb_latency_per_level = new SubsecondTime[tlb_subsystem->getTLBSubsystem().size()];

		// Keep track of the tlb latency for each TLB level
		for (UInt32 i = 0; i < tlb_subsystem->getTLBSubsystem().size(); i++){
			registerStatsMetric(name, core->getId(), "tlb_latency_" + itostr(i), &translation_stats.tlb_latency_per_level[i]);
		}
			
	}

	/**
	 * @brief Perform address translation from virtual to physical address.
	 *
	 * This function performs address translation for a given virtual address.
	 * It also updates various translation statistics and
	 * performance models.
	 *
	 * @param eip The instruction pointer.
	 * @param address The virtual address to be translated.
	 * @param instruction A flag indicating if the address is for an instruction.
	 * @param lock The lock signal (this is used across Sniper)
	 * @param modeled A flag indicating if the translation should be modeled (only true if detailed memory modeling is enabled). 
	 * @param count A flag indicating if the translation should be counted in statistics (usually modeled & count are true when detailed memory modeling is enabled).
	 * @return A pair containing the total latency of the translation and the translated physical address.
	 */

	IntPtr MemoryManagementUnit::performAddressTranslation(IntPtr eip, IntPtr address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count)
	{
		// We keep track of the number of accesses to the DRAM during the translation process
		dram_accesses_during_last_walk = 0;

#ifdef DEBUG_MMU
		log_file << std::endl;
		log_file << "[MMU] ---- Starting address translation for virtual address: " << address <<  " ---- at time " << shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD) << std::endl;
#endif

		SubsecondTime time = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD); 

		// Cleanup so that we do not have any completed entries left inside the MSHRs of the PT walkers
		pt_walkers->removeCompletedEntries(time);

		if (count)
			translation_stats.num_translations++;

		TLBSubsystem tlbs = tlb_subsystem->getTLBSubsystem(); // Get the TLB hierarchy

		//  This is the time we start the translation process 
		// If there is a metadata table, we need to walk it to get the metadata information


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
#ifdef DEBUG_MMU
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

					tlb_block_info = tlbs[i][j]->lookup(address, time, count, lock, eip, modeled, count);

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
						tlb_block_info = tlbs[i][j]->lookup(address, time, count, lock, eip, modeled, count);
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
#ifdef DEBUG_MMU
			log_file << "[MMU] TLB Hit at level: " << hit_level << " at TLB " << hit_tlb->getName() << std::endl;
#endif
				break;
			}
		}


		SubsecondTime charged_tlb_latency = SubsecondTime::Zero();
		

		/* @kanellok: FOCA Lab 4 Tips and Hints: 

		We performed the TLB lookup and we now need to decide what latency to charge based on whether we had a TLB hit or a TLB miss.
		
		If it is an L2 TLB hit, you need to change the code below to take into account the page size prediction and the page size used in the TLB hit.
		*/

		//If we have a TLB hit, we need to charge the TLB hit latency
		if (hit)
		{
			ppn_result = tlb_block_info_hit->getPPN(); // We get the PPN from the block info
			page_size = tlb_block_info_hit->getPageSize(); // We get the page size from the block info
			#ifdef DEBUG_MMU
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
#ifdef DEBUG_MMU
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

#ifdef DEBUG_MMU
					log_file << "[MMU] Charging TLB Hit Latency: " << hit_tlb->getLatency() << " at level: " << hit_level << std::endl;
#endif
				}
			}

			// Progress the clock to the time after the TLB latency
			// This is done so that the PTW starts after the TLB latency
			// @kanellok: Be very careful if you want to play around with the timing model
			shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time + charged_tlb_latency); 
			#ifdef DEBUG_MMU
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
			
#ifdef DEBUG_MMU
			log_file << "[MMU] TLB Miss" << std::endl;
#endif
			for (UInt32 i = 0; i < tlbs.size(); i++) 
			{
				for (UInt32 j = 0; j < tlbs[i].size(); j++)
				{
					tlb_latency[i] = max(tlbs[i][j]->getLatency(), tlb_latency[i]);
				}
#ifdef DEBUG_MMU
				log_file << "[MMU] Charging TLB Latency: " << tlb_latency[i] << " at level: " << i << std::endl;
#endif
				translation_stats.total_tlb_latency += tlb_latency[i];
				charged_tlb_latency += tlb_latency[i];
			}
			
			// We progress the time by the charged TLB latency so that the PTW starts after the TLB latency
			shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time + charged_tlb_latency);
			#ifdef DEBUG_MMU
				log_file << "[MMU] New time after charging TLB latency: " << shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD) << std::endl;
			#endif
		}

		// We need to keep track of the total walk latency and the total fault latency (if there was a fault)
		SubsecondTime total_walk_latency = SubsecondTime::Zero();
		SubsecondTime total_fault_latency = SubsecondTime::Zero();



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
			#ifdef DEBUG_MMU
				log_file << "[MMU] New time after charging the PT walker allocation delay: " << shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD) << std::endl;
			#endif

			// returns PTW latency, PF latency, Physical Address, Page Size as a tuple
			int app_id = core->getThread()->getAppId();
			PageTable* page_table = Sim()->getMimicOS()->getPageTable(app_id);

			const bool restart_walk_upon_page_fault = false;

			#ifdef DEBUG_MMU
				log_file << "[MMU] Starting PTW at time: " << shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD).getNS() << std::endl;
			#endif
			
			auto ptw_result = performPTW(address, modeled, count, false, eip, lock, page_table, restart_walk_upon_page_fault);
			total_walk_latency = get<0>(ptw_result); // Total walk latency is only the time it takes to walk the page table (excluding page faults)

			#ifdef DEBUG_MMU
				log_file << "[MMU] PTW Completed at time: " << shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD).getNS() + total_walk_latency.getNS() << std::endl;
			#endif
			
			if (count)
			{
				translation_stats.total_walk_latency += total_walk_latency;
				translation_stats.page_table_walks++;
			}

			// If the page fault latency is greater than zero, we need to charge the page fault latency
			bool caused_page_fault = get<1>(ptw_result);
			

			if (caused_page_fault)
			{

				int max_level = Sim()->getMimicOS()->getPageTable(app_id)->getMaxLevel();
				Sim()->getMimicOS()->handle_page_fault(address, app_id, max_level);
				
				SubsecondTime m_page_fault_latency = Sim()->getMimicOS()->getPageFaultLatency();	
				if (count)
				{
					translation_stats.page_faults++;
					translation_stats.total_fault_latency += m_page_fault_latency;
				}
				total_fault_latency = m_page_fault_latency;

				auto restarted_ptw_result = performPTW(address, modeled, count, true, eip, lock, page_table, false);
			
				total_walk_latency += get<0>(restarted_ptw_result);
			
				page_size = get<3>(restarted_ptw_result);
				ppn_result = get<2>(restarted_ptw_result);

				#ifdef DEBUG_MMU
					log_file << "[MMU] Restarted PTW Result: " << get<0>(restarted_ptw_result) << " " << get<1>(restarted_ptw_result) << " " << get<2>(restarted_ptw_result) << " " << get<3>(restarted_ptw_result) << std::endl;	
					log_file << "[MMU] VPN: " << (address >> page_size) << std::endl;
					log_file << "[MMU] PPN: " << ppn_result << std::endl;
					log_file << "[MMU] Page Size: " << page_size << std::endl;
				#endif

			}
			else{

				ppn_result = get<2>(ptw_result);
				page_size = get<3>(ptw_result);

			}



			/*
			We need to set the completion time:
			1) Time before PTW starts
			2) Delay because of all the walkers being busy
			3) Total walk latency
			4) Total fault latency
			*/

			pt_walker_entry.completion_time = time_for_pt + delay + total_walk_latency + total_fault_latency;
			pt_walkers->allocate(pt_walker_entry);

			#ifdef DEBUG_MMU
				log_file << "[MMU] PTW Completion Time: " << pt_walker_entry.completion_time.getNS() << std::endl;
			#endif

			/* 
			We need to set the time to the time after the PTW is completed. 
			This is done so that the memory manager sends the request to the cache hierarchy after the PTW is completed
			*/

			if (caused_page_fault){
				PseudoInstruction *i = new PageFaultRoutineInstruction(total_fault_latency);
				getCore()->getPerformanceModel()->queuePseudoInstruction(i);
				shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, pt_walker_entry.completion_time);
			}
			else{
				shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, pt_walker_entry.completion_time);
			}




			#ifdef DEBUG_MMU
				log_file << "[MMU] New time after charging the PT walker completion time: " << shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD) << std::endl;
			#endif

		}


#ifdef DEBUG_MMU
		log_file << "[MMU] Total Walk Latency: " << total_walk_latency << std::endl;
		log_file << "[MMU] Total Fault Latency: " << total_fault_latency << std::endl;
#endif

		// instruction path: follows the instruction TLB path
		// data path: follows the data TLB path
		if (instruction)
			tlbs = tlb_subsystem->getInstructionPath();
		else
			tlbs = tlb_subsystem->getDataPath();

		std::map<int, vector<tuple<IntPtr, int>>> evicted_translations;

		// We need to allocate the entry in every "allocate on miss" TLB

		int tlb_levels = tlbs.size();


		for (int i = 0; i < tlb_levels; i++)
		{
			// We will check where we need to allocate the page

			for (UInt32 j = 0; j < tlbs[i].size(); j++)
			{
				// We need to check if there are any evicted translations from the previous level and allocate them
				if ((i > 0) && (evicted_translations[i - 1].size() != 0))
				{
					tuple<bool, IntPtr, int> result;

#ifdef DEBUG_MMU
					log_file << "[MMU] There are evicted translations from level: " << i - 1 << std::endl;
#endif
					// iterate through the evicted translations and allocate them in the current TLB
					for (UInt32 k = 0; k < evicted_translations[i - 1].size(); k++)
					{
#ifdef DEBUG_MMU
						log_file << "[MMU] Evicted Translation: " << get<0>(evicted_translations[i - 1][k]) << std::endl;
#endif
						// We need to check if the TLB supports the page size of the evicted translation
						if (tlbs[i][j]->supportsPageSize(page_size))
						{
#ifdef DEBUG_MMU

							log_file << "[MMU] " << tlbs[i][j]->getName() << " supports page size: " << page_size << std::endl;
							log_file << "[MMU] Allocating in TLB: Level = " << i << " Index = " << j << " with page size: " << page_size << " and VPN: " << (get<0>(evicted_translations[i - 1][k]) >> page_size) << std::endl;
#endif

							result = tlbs[i][j]->allocate(get<0>(evicted_translations[i - 1][k]), time, count, lock, get<1>(evicted_translations[i - 1][k]), ppn_result);

							// If the allocation was successful and we have an evicted translation, 
							// we need to add it to the evicted translations vector for

							if (get<0>(result) == true)
							{
								evicted_translations[i].push_back(make_tuple(get<1>(result), get<2>(result)));
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
					
#ifdef DEBUG_MMU
					log_file << "[MMU] " << tlbs[i][j]->getName() << " supports page size: " << page_size << std::endl;
					log_file << "[MMU] Allocating in TLB: Level = " << i << " Index = " << j << " with page size: " << page_size << " and VPN: " << (address >> page_size) << std::endl;
#endif
					tuple<bool, IntPtr, int> result;

					result = tlbs[i][j]->allocate(address, time, count, lock, page_size, ppn_result);
					if (get<0>(result) == true)
					{
						evicted_translations[i].push_back(make_tuple(get<1>(result), get<2>(result)));
					}
				}

			}
		}

		
		translation_stats.total_translation_latency += charged_tlb_latency + total_walk_latency;

		// We need to calculate the physical address
		// The physical address is the PPN * page size + offset: the PTW always returns the PPN at the page granularity (e.g. 4KB)
		// The offset is the last 12/21 bits of the address which we get by doing (address % page_size_in_bytes)
		// The page size is either 4KB or 2MB

		int page_size_in_bytes = pow(2, page_size);
		int base_page_size_in_bytes = pow(2, 12);

#ifdef DEBUG_MMU
		if(charged_tlb_latency > total_walk_latency && total_walk_latency > SubsecondTime::Zero()){
			std::cout << "[MMU] TLB latency is greater than the page table walk latency" << std::endl;
			
			UInt64  elapsed_time = charged_tlb_latency.getNS();
			UInt64 freq = (UInt64) (core->getDvfsDomain()->getPeriodInFreqMHz());
			//calculate the elapsed cycles - frequency is in MHz
			UInt64 cycles = (UInt64) (elapsed_time * freq / 1000);

			std::cout << "[MMU] TLB latency: " << cycles << " cycles" << std::endl;

			elapsed_time = total_walk_latency.getNS();
			//calculate the elapsed cycles - frequency is in MHz
			cycles = (UInt64) (elapsed_time * freq / 1000);
			std::cout << "[MMU] Page table walk latency: " << cycles << " cycles" << std::endl;
			
		}
#endif
		IntPtr final_physical_address = (ppn_result * base_page_size_in_bytes) + (address % page_size_in_bytes);

		#ifdef DEBUG_MMU
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

	PTWResult MemoryManagementUnit::filterPTWResult(PTWResult ptw_result, PageTable *page_table, bool count)
	{
		accessedAddresses ptw_accesses;

		if (m_pwc_enabled)
		{
			accessedAddresses original_ptw_accesses = get<1>(ptw_result);
			// We need to filter based on the page walk caches
			for (UInt32 i = 0; i < get<1>(ptw_result).size(); i++)
			{
				bool pwc_hit = false;

					// We need to check if the entry is in the PWC
					// If it is, we need to remove it from the PTW result
					// If it is not, we need to add it to the PTW result
					// Only check page walk caches if the level is not the first one

					int level = get<1>(original_ptw_accesses[i]);

					IntPtr pwc_address = get<2>(original_ptw_accesses[i]);
#ifdef DEBUG_MMU
					log_file << "[MMU] Checking PWC for address: " << pwc_address << " at level: " << level << std::endl;
#endif
					if (level < max_pwc_level){
						pwc_hit = pwc->lookup(pwc_address, SubsecondTime::Zero(), true, level, count);
					}
					

#ifdef DEBUG_MMU
					log_file << "[MMU] PWC HIT: " << pwc_hit << " level: " << level << std::endl;
#endif
					// If the entry is not in the cache, we need to access the memory

					if (!pwc_hit)
					{
						// The entry is stored in: current_frame->emulated_ppn * 4096 which is the physical address of the frame
						// The offset is the index of the entry in the frame
						// The size of the entry is 8 bytes
						// The physical address of the entry is: current_frame->emulated_ppn * 4096 + offset*8
						ptw_accesses.push_back(get<1>(ptw_result)[i]);
					}
			}
		}

		return PTWResult(get<0>(ptw_result), ptw_accesses, get<2>(ptw_result), get<3>(ptw_result), get<4>(ptw_result));
	}

	void MemoryManagementUnit::setMaxPWCLevel(int max_pwc_level_)
	{
		max_pwc_level = max_pwc_level_;
	}

	int MemoryManagementUnit::getMaxPWCLevel()
	{
		return max_pwc_level;
	}

	/*
	We do not need to use VMAs in the current implementation of the MMU
	*/
	void MemoryManagementUnit::discoverVMAs()
	{
		return ;
	}

}