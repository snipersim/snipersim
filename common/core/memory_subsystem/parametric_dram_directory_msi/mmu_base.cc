

#include "./memory_manager.h"
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "shmem_perf_model.h"
#include "pagetable.h"
#include "tlb_subsystem.h"
#include "cache_block_info.h"
#include "stats.h"
#include "mimicos.h"

//#define DEBUG_MMU

namespace ParametricDramDirectoryMSI
{

	MemoryManagementUnitBase::MemoryManagementUnitBase(Core *_core, MemoryManager *_memory_manager, ShmemPerfModel *_shmem_perf_model, String _name) : core(_core),
	memory_manager(_memory_manager),
	shmem_perf_model(_shmem_perf_model),
	name(_name)
	{
		log_file_mmu = std::ofstream();

		log_file_name_mmu = "mmu_walker.log." + std::to_string(core->getId());
		log_file_name_mmu = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + log_file_name_mmu;
		log_file_mmu.open(log_file_name_mmu.c_str());
	}


	/**
	 * @brief Accesses the cache and performs address translation if a nested MMU is present.
	 *
	 * This function handles the cache access for a given translation packet. If a nested MMU is present,
	 * it performs address translation and updates the packet address accordingly. It then accesses the
	 * L1 data cache or performs a prefetch operation if specified.
	 *
	 * @param packet The translation packet containing the address and other relevant information.
	 * @param t_start The start time of the cache access operation.
	 * @param is_prefetch A boolean flag indicating whether the operation is a prefetch.
	 * @return The latency of the cache access operation.
	 */
	SubsecondTime MemoryManagementUnitBase::accessCache(translationPacket packet, SubsecondTime t_start, bool is_prefetch)
	{

#ifdef DEBUG_MMU
		log_file_mmu << std::endl;
		log_file_mmu << "[MMU_BASE] ---- Starting cache access from MMU " << std::endl;
#endif 
		SubsecondTime host_translation_latency = SubsecondTime::Zero();
		IntPtr host_physical_address = packet.address;

		// Snapshot the current time
		SubsecondTime t_temp = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

		
		IntPtr cache_address = ((IntPtr)(packet.address)) & (~((64 - 1)));
		CacheCntlr *l1d_cache = memory_manager->getCacheCntlrAt(core->getId(), MemComponent::component_t::L1_DCACHE);

		// Update the elapsed time in the performance model so that we send the request to the cache at the correct time
		// Example: we need to access 4 levels of the page table:
		// 1. Access the first level of the page table at time t_start
		// 2. Access the second level of the page table at time t_start + latency of the first level
		// 3. Access the third level of the page table at time t_start + latency of the first level + latency of the second level
		// ..

		shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, t_start);

#ifdef DEBUG_MMU
		log_file_mmu << "[MMU_BASE] Accessing cache with  address: " << packet.address << " at time " << t_start << std::endl;
#endif 
		HitWhere::where_t hit_where = HitWhere::UNKNOWN;
		if(is_prefetch){

#ifdef DEBUG_MMU
			log_file_mmu << "Prefetching address: " << packet.address << " at time " << t_start << std::endl;
#endif
			IntPtr cache_address = ((IntPtr)(packet.address)) & (~((64 - 1)));

			CacheCntlr *l2_cache = memory_manager->getCacheCntlrAt(core->getId(), MemComponent::L2_CACHE);
			l2_cache->doPrefetch(packet.eip, cache_address, shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD), CacheBlockInfo::block_type_t::PAGE_TABLE);

		}
		else {
			hit_where = l1d_cache->processMemOpFromCore(
			packet.eip,
			packet.lock_signal,
			Core::mem_op_t::READ,
			cache_address, 0,
			NULL, 8,
			packet.modeled,
			packet.count, packet.type, host_translation_latency);

#ifdef DEBUG_MMU
			log_file_mmu << "[MMU_BASE] Cache hit where: " << HitWhereString(hit_where) << std::endl;
#endif
			if (HitWhereString(hit_where) == std::string("dram-local"))
				dram_accesses_during_last_walk++;
		}

		SubsecondTime t_end = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

		// Restore the elapsed time in the performance model
		shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, t_temp);

		// Tag the cache block with the block type (e.g., page table data)
		memory_manager->tagCachesBlockType(packet.address, packet.type);
#ifdef DEBUG_MMU
		log_file_mmu << "[MMU_BASE] ---- Finished cache access from MMU " << std::endl;
		log_file_mmu << std::endl;
#endif

		return t_end - t_start;
	}


	/**
	 * @brief Calculate the Page Table Walk (PTW) cycles for a given PTW result.
	 *
	 * This function calculates the latency incurred during a page table walk based on the provided PTW result.
	 * It iterates through the accessed addresses, determines the levels and tables involved, and computes the
	 * latency for each table and level. The total walk latency is then returned.
	 *
	 * @param ptw_result The result of the page table walk, containing accessed addresses and other information.
	 * @param count A boolean flag indicating whether to count the cycles.
	 * @param modeled A boolean flag indicating whether the PTW is modeled.
	 * @param eip The instruction pointer address.
	 * @param lock The lock signal for the core.
	 * @return SubsecondTime The total latency incurred during the page table walk.
	 */
	SubsecondTime MemoryManagementUnitBase::calculatePTWCycles(PTWResult ptw_result, bool count, bool modeled, IntPtr eip, Core::lock_signal_t lock)
	{

		accessedAddresses accesses = get<1>(ptw_result);

		translationPacket packet;
		packet.eip = eip; 
		packet.instruction = false;
		packet.lock_signal = lock;
		packet.modeled = modeled;
		packet.count = count;
		packet.type = CacheBlockInfo::block_type_t::PAGE_TABLE;

		SubsecondTime latency = SubsecondTime::Zero();

		int levels = 0;
		int tables = 0;

		for (UInt32 i = 0; i < accesses.size(); i++)
		{
			int level = get<1>(accesses[i]);
			int table = get<0>(accesses[i]);
			if (level > levels)
				levels = level;
			if (table > tables)
				tables = table;
		}

		SubsecondTime latency_per_table_per_level[tables + 1][levels + 1] = {SubsecondTime::Zero()};

		int correct_table = -1;
		int correct_level = -1;

		/* iterate through the accesses and calculate the latency for each table and level */
		SubsecondTime t_now =  shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

#ifdef DEBUG_MMU
		log_file_mmu << "[MMU_BASE] Starting PTW at time: " << t_now << std::endl;
		log_file_mmu << "[MMU_BASE] We need to access " << accesses.size() << " addresses" << std::endl;
#endif

		// There are two options here: we will either charge the latency of the page fault before the PTW or after the PTW
		// In this case, we will charge the page fault latency after the PTW -> this will happen in the mmu_base.cc
		// The PTW requests will be scheduled in the cache before the page fault -> the effect of this is minimal
		// Optimally: we should charge the page fault latency before the PTW 



		// You have parallel requests across multiple tables 

		// Example 
		// Table for 4KB -> Level 1 <--[Fetch Delay]--> Level 2 
		// Table for 2MB -> Level 1 <----------[Fetch Delay]----------> Level 2 

		// We need to be keeping track of the delay for each table and level


		SubsecondTime fetch_delay[tables + 1];
		for (int i = 0; i < (tables + 1); i++)
		{
			fetch_delay[i] = SubsecondTime::Zero();
		}

		SubsecondTime correct_access_translation_time = SubsecondTime::MaxTime();

		for (int level = 0; level < (levels + 1); level++)
		{
#ifdef DEBUG_MMU
			log_file_mmu << "[MMU_BASE] We start performing all the accesses for level: " << level << std::endl;
#endif
			for (int tab = 0; tab < (tables + 1); tab++)
			{

				for (UInt32 req = 0; req < accesses.size(); req++)
				{
					int current_level = get<1>(accesses[req]);
					int current_table = get<0>(accesses[req]);
					IntPtr current_address = get<2>(accesses[req]);
					if (current_level == level && current_table == tab)
					{
				
						// If the request is going to be scheduled to the cache hierarchy AFTER the time that the correct translation is available, 
						// we can skip the access, as it will be redundant
						if(t_now+fetch_delay[tab] < correct_access_translation_time)
						{
							packet.address = current_address;
							latency = accessCache(packet, t_now+fetch_delay[tab]);
						}
						else{
							latency = SubsecondTime::Zero();
						}
#ifdef DEBUG_MMU
			log_file_mmu << "[MMU_BASE] We accessed address: " << current_address << " from level: " << level << " in table: " << tab << " at time: " << t_now+fetch_delay[tab] << " with latency: " << latency << std::endl;
#endif
					
						if (get<3>(accesses[req]) == true)
						{
							correct_table = get<0>(accesses[req]);
							correct_level = get<1>(accesses[req]);
							correct_access_translation_time = t_now + fetch_delay[tab] + latency;
							latency_per_table_per_level[current_table][current_level] = latency;
						}
						else if (latency_per_table_per_level[current_table][current_level] < latency && current_level != correct_level)
						{
							latency_per_table_per_level[current_table][current_level] = latency;
						}

					}
				}
				// We need to update the fetch delay for the next level 
				fetch_delay[tab] += latency_per_table_per_level[tab][level];
#ifdef DEBUG_MMU
				log_file_mmu << "[MMU_BASE] Finished PTW for table: " << tab << " and level " << level << " at time: " << t_now+fetch_delay[tab] << std::endl;
#endif
			}
		}

#ifdef DEBUG_MMU
		log_file_mmu << "[MMU_BASE] Let's try to find the latency for the correct translation" << std::endl;
#endif

		// Walking the correct table leads to the calculation of the total walk latency
		// The requests for the other tables will be sent to the cache hierarchy to model the contention 
		// but they will not be charged in the total walk latency

		SubsecondTime walk_latency = SubsecondTime::Zero();

		if (correct_table != -1)
		{
#ifdef DEBUG_MMU
			log_file_mmu << "[MMU_BASE] We found the correct translation" << std::endl;
			log_file_mmu << "[MMU_BASE] Correct table: " << correct_table << " Correct level: " << correct_level << std::endl;
			log_file_mmu << "[MMU_BASE] The latency for the correct translation depends only on the correct table" << std::endl;
#endif
			for (int level = 0; level < (levels + 1); level++)
			{

				walk_latency+=latency_per_table_per_level[correct_table][level];
	#ifdef DEBUG_MMU
				log_file_mmu << "[MMU_BASE] We are adding the latency for level: " << level << " in table: " << correct_table << " with latency: " << latency_per_table_per_level[correct_table][level] << std::endl;
				log_file_mmu << "[MMU_BASE] Updated walk latency: " << walk_latency << std::endl;
	#endif
			}
		}
		else {
#ifdef DEBUG_MMU
			log_file_mmu << "[MMU_BASE] We did not find the correct translation" << std::endl;
			log_file_mmu << "[MMU_BASE] We will return the maximum latency based on \"slowest\" table"  << std::endl;
#endif
			SubsecondTime max_latency = SubsecondTime::Zero();
			for (int tab = 0; tab < (tables + 1); tab++)
			{
				for (int level = 0; level < (levels + 1); level++)
				{
					walk_latency += latency_per_table_per_level[tab][level];
				}

				if (walk_latency > max_latency)
				{
					max_latency = walk_latency;
				}
			}

		}

		return walk_latency;
	}

	
	/**
	 * @brief Perform a Page Table Walk (PTW) for a given address.
	 *
	 * This function initiates a page table walk for the specified address and returns the result
	 * as a tuple containing the time taken for the PTW, the time taken for page fault handling (if any),
	 * the physical page number (PPN) resulting from the PTW, and the page size.
	 *
	 * @param address The virtual address for which the PTW is performed.
	 * @param modeled A boolean indicating whether the PTW should be modeled.
	 * @param count A boolean indicating whether the PTW should be counted.
	 * @param is_prefetch A boolean indicating whether the PTW is for a prefetch operation.
	 * @param eip The instruction pointer (EIP) at the time of the PTW.
	 * @param lock The lock signal for the core.
	 * @return A tuple containing:
	 *         - The time taken for the PTW (SubsecondTime).
	 *         - Whether a page fault occurred (bool).
	 *         - The physical page number (IntPtr) resulting from the PTW (at the 4KB granularity).
	 *         - The page size (int).
	 */
	tuple<SubsecondTime, bool, IntPtr, int> MemoryManagementUnitBase::performPTW(IntPtr address, bool modeled, bool count, bool is_prefetch, IntPtr eip, Core::lock_signal_t lock, PageTable *page_table, bool restart_walk)
	{

#ifdef DEBUG_MMU
			log_file_mmu << std::endl;
			log_file_mmu << "[MMU_BASE]-------------- Starting PTW for address: " << address << std::endl;
#endif
			auto ptw_result = page_table->initializeWalk(address, count, is_prefetch, restart_walk);

			

			// We will filter out the re-walked addresses which anyways either hit in the PWC or are redundant
			accessedAddresses visited_pts = get<1>(ptw_result);
			std::sort(visited_pts.begin(), visited_pts.end());
			visited_pts.erase(std::unique(visited_pts.begin(), visited_pts.end()), visited_pts.end());


			ptw_result = make_tuple(get<0>(ptw_result), visited_pts, get<2>(ptw_result), get<3>(ptw_result), get<4>(ptw_result));

			// Filter the PTW result based on the page table type
			// This filtering is necessary to remove any redundant accesses that may hit in the PWC

			if ((page_table->getType() == "radix"))
			{
				ptw_result = filterPTWResult(ptw_result, page_table, count);
			}

#ifdef DEBUG_MMU
			log_file_mmu << "[MMU_BASE] We accessed " << get<1>(ptw_result).size() << " addresses" << std::endl;
			visited_pts = get<1>(ptw_result);
			for (UInt32 i = 0; i < visited_pts.size(); i++)
			{
				log_file_mmu << "[MMU_BASE] Address: " << get<2>(visited_pts[i]) << " Level: " << get<1>(visited_pts[i]) << " Table: " << get<0>(visited_pts[i]) << " Correct Translation: " << get<3>(visited_pts[i]) << std::endl;
			}
#endif

			int page_size = get<0>(ptw_result);
			IntPtr ppn_result = get<2>(ptw_result);
			bool is_pagefault = get<4>(ptw_result);

			
			SubsecondTime ptw_cycles = calculatePTWCycles(ptw_result, count, modeled, eip, lock);
			
		#ifdef DEBUG_MMU
			log_file_mmu << "[MMU_BASE] Finished PTW for address: " << address << std::endl;
			log_file_mmu << "[MMU_BASE] PTW latency: " << ptw_cycles << std::endl;
			log_file_mmu << "[MMU_BASE] Physical Page Number: " << ppn_result << std::endl;
			log_file_mmu << "[MMU_BASE] Page Size: " << page_size << std::endl;
			log_file_mmu << "[MMU_BASE] -------------- End of PTW" << std::endl;
		#endif

			return make_tuple(ptw_cycles, is_pagefault, ppn_result, page_size);

	}

}