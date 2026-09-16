

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
#include "simulator.h"
#include "config.hpp"
#include "sim_log.h"

#include <algorithm>

#include "debug_config.h"

// ============================================================================
// Compile-Time Configuration
// ============================================================================



namespace ParametricDramDirectoryMSI
{

	MemoryManagementUnitBase::MemoryManagementUnitBase(Core *_core, MemoryManagerBase *_memory_manager, ShmemPerfModel *_shmem_perf_model, String _name, MemoryManagementUnitBase* _nested_mmu) : core(_core),
		memory_manager(_memory_manager),
		shmem_perf_model(_shmem_perf_model),
		name(_name),
		nested_mmu(_nested_mmu),
		dram_accesses_during_last_walk(0),
		m_ptw_id_counter(0)
    {
        std::cout << "[MMU_BASE] Initializing MMU Base for core " << core->getId() << " with name: " << name << std::endl;
        
        // Initialize SimLog for mmu_base (uses DEBUG_MMU_BASE flag)
        mmu_base_log = new SimLog("MMU_BASE", core->getId(), DEBUG_MMU_BASE);
        
        count_page_fault_latency_enabled = Sim()->getCfg()->getBool("perf_model/"+_name+"/count_page_fault_latency");
        std::cout << "[MMU_BASE] Count page fault latency enabled: " << (count_page_fault_latency_enabled ? "Yes" : "No") << std::endl;

        // Perfect translation mode: translation happens (PA remapping) but with zero latency
        perfect_translation_enabled = Sim()->getCfg()->getBoolDefault("perf_model/"+_name+"/perfect_translation", false);
        if (perfect_translation_enabled) {
            std::cout << "[MMU_BASE] PERFECT TRANSLATION MODE ENABLED - zero translation latency" << std::endl;
        }

        // Per-access PTW logging (runtime config option)
        ptw_access_logging_enabled = Sim()->getCfg()->getBoolDefault("perf_model/"+_name+"/ptw_access_logging", false);
        try {
            ptw_access_logging_sample_rate = Sim()->getCfg()->getInt("perf_model/"+_name+"/ptw_access_logging_sample_rate");
        } catch (...) {
            ptw_access_logging_sample_rate = 100;  // Default sample rate
        }
        ptw_access_log_counter = 0;
        
        if (ptw_access_logging_enabled) {
            std::string ptw_access_log_filename = "ptw_access_log_" + std::to_string(core->getId()) + ".csv";
            ptw_access_log_filename = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + ptw_access_log_filename;
            ptw_access_log_file.open(ptw_access_log_filename.c_str());
            if (ptw_access_log_file.is_open()) {
                // Header: access_num,ptw_id,vpn,level,table,cache_line_addr,l1_hit,l2_hit,nuca_hit,dram_hit,latency_ns,is_pte
                ptw_access_log_file << "access_num,ptw_id,vpn,level,table,cache_line_addr,l1_hit,l2_hit,nuca_hit,dram_hit,latency_ns,is_pte" << std::endl;
                std::cout << "[MMU_BASE] PTW per-access logging enabled (sample rate: 1/" << ptw_access_logging_sample_rate << ")" << std::endl;
            }
        }

        // <--- CHANGED: Open the dump file and write the CSV header
		#if ENABLE_MMU_CSV_LOGS
			ptw_dump_filename = "ptw_dump_" + std::to_string(core->getId()) + ".csv";
			ptw_dump_filename = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + ptw_dump_filename;
			ptw_dump_file.open(ptw_dump_filename.c_str());
		#endif
        
        // Header: Address, EIP, TLB hits between PTWs, then cache-level counts
		#if ENABLE_MMU_CSV_LOGS
			ptw_dump_file << "PTW_Address,EIP,TLB_Hits_Since_Last_PTW,Count_L1D,Count_L2,Count_NUCA,Count_DRAM" << std::endl;
		#endif
		tlb_hits_since_last_ptw = 0;
        // <--- END CHANGED

        stats.memory_accesses = 0;
        stats.memory_accesses_before_filtering = 0;

        walker_stats.DRAM_accesses = 0;
        walker_stats.DRAM_accesses_local = 0;
        walker_stats.DRAM_accesses_remote = 0;
        for (UInt32 i = 0; i < MMU_MAX_NUMA_NODES; i++)
            walker_stats.DRAM_accesses_per_numa_node[i] = 0;
        walker_stats.L1D_accesses = 0;
        walker_stats.L2_accesses = 0;
        walker_stats.NUCA_accesses = 0;
        walker_stats.DRAM_accesses_prefetch = 0;
        walker_stats.L1D_accesses_prefetch = 0;
        walker_stats.L2_accesses_prefetch = 0;
        walker_stats.NUCA_accesses_prefetch = 0;

        // Read NUMA configuration
        m_numa_enabled = false;
        m_num_numa_nodes = 1;
        try {
            m_numa_enabled = Sim()->getCfg()->getBool("perf_model/dram/numa/enabled");
        } catch (...) {
            m_numa_enabled = false;
        }
        if (m_numa_enabled) {
            try {
                m_num_numa_nodes = Sim()->getCfg()->getInt("perf_model/dram/numa/num_nodes");
            } catch (...) {
                m_num_numa_nodes = 2;
            }
            if (m_num_numa_nodes > MMU_MAX_NUMA_NODES)
                m_num_numa_nodes = MMU_MAX_NUMA_NODES;
        }
        
        registerStatsMetric("mmu_walker", core->getId(), "memory_accesses", &stats.memory_accesses);
        registerStatsMetric("mmu_walker", core->getId(), "memory_accesses_before_filtering", &stats.memory_accesses_before_filtering);

        registerStatsMetric("mmu_walker", core->getId(), "DRAM_accesses", &walker_stats.DRAM_accesses);
        registerStatsMetric("mmu_walker", core->getId(), "DRAM_accesses_local", &walker_stats.DRAM_accesses_local);
        registerStatsMetric("mmu_walker", core->getId(), "DRAM_accesses_remote", &walker_stats.DRAM_accesses_remote);
        for (UInt32 i = 0; i < m_num_numa_nodes; i++) {
            String numa_stat_name = "DRAM_accesses_numa_node" + String(std::to_string(i).c_str());
            registerStatsMetric("mmu_walker", core->getId(), numa_stat_name, &walker_stats.DRAM_accesses_per_numa_node[i]);
        }
        registerStatsMetric("mmu_walker", core->getId(), "L1D_accesses", &walker_stats.L1D_accesses);
        registerStatsMetric("mmu_walker", core->getId(), "L2_accesses", &walker_stats.L2_accesses);
        registerStatsMetric("mmu_walker", core->getId(), "NUCA_accesses", &walker_stats.NUCA_accesses);
        registerStatsMetric("mmu_walker", core->getId(), "DRAM_accesses_prefetch", &walker_stats.DRAM_accesses_prefetch);
        registerStatsMetric("mmu_walker", core->getId(), "L1D_accesses_prefetch", &walker_stats.L1D_accesses_prefetch);
        registerStatsMetric("mmu_walker", core->getId(), "L2_accesses_prefetch", &walker_stats.L2_accesses_prefetch);
        registerStatsMetric("mmu_walker", core->getId(), "NUCA_accesses_prefetch", &walker_stats.NUCA_accesses_prefetch);
    }

	MemoryManagementUnitBase::~MemoryManagementUnitBase()
	{
		if (mmu_base_log) {
			delete mmu_base_log;
			mmu_base_log = nullptr;
		}
		// Close per-access PTW log file
		if (ptw_access_log_file.is_open()) {
			ptw_access_log_file.close();
		}
	}
	
	/**
	 * @brief Log individual PTW cache access to CSV file
	 * 
	 * Default implementation for base MMU class. Logs per-access PTW data when enabled.
	 * Override in derived classes for custom logging.
	 */
	void MemoryManagementUnitBase::logPTWCacheAccess(UInt64 ptw_id, UInt64 vpn, int level, int table,
	                                                  IntPtr cache_line_addr, HitWhere::where_t hit_where,
	                                                  SubsecondTime latency, bool is_pte)
	{
		if (!ptw_access_logging_enabled || !ptw_access_log_file.is_open()) {
			return;
		}
		
		// Sample logging to reduce overhead
		if (++ptw_access_log_counter % ptw_access_logging_sample_rate != 0) {
			return;
		}
		
		// Determine hit location
		int l1_hit = 0, l2_hit = 0, nuca_hit = 0, dram_hit = 0;
		if (hit_where == HitWhere::where_t::L1_OWN) {
			l1_hit = 1;
		} else if (hit_where == HitWhere::where_t::L2_OWN) {
			l2_hit = 1;
		} else if (hit_where == HitWhere::where_t::NUCA_CACHE) {
			nuca_hit = 1;
		} else if (hit_where == HitWhere::where_t::DRAM_LOCAL || hit_where == HitWhere::where_t::DRAM) {
			dram_hit = 1;
		}
		
		// Format: access_num,ptw_id,vpn,level,table,cache_line_addr,l1_hit,l2_hit,nuca_hit,dram_hit,latency_ns,is_pte
		ptw_access_log_file << ptw_access_log_counter << ","
		                    << ptw_id << ","
		                    << vpn << ","
		                    << level << ","
		                    << table << ","
		                    << cache_line_addr << ","
		                    << l1_hit << ","
		                    << l2_hit << ","
		                    << nuca_hit << ","
		                    << dram_hit << ","
		                    << latency.getNS() << ","
		                    << (is_pte ? 1 : 0) << std::endl;
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
	SubsecondTime MemoryManagementUnitBase::accessCache(translationPacket packet, SubsecondTime t_start, bool is_prefetch, HitWhere::where_t& out_hit_where)
	{

		mmu_base_log->debug("---- Starting cache access from MMU");
		SubsecondTime host_translation_latency = SubsecondTime::Zero();
		IntPtr host_physical_address = packet.address;
		// If there is a nested MMU, perform address translation to translate the guest physical address to the host physical address
		if (nested_mmu != nullptr){


			auto host_translation_result = nested_mmu->performAddressTranslation(packet.eip, packet.address, packet.instruction, packet.lock_signal , packet.modeled, packet.count);
			host_physical_address = host_translation_result;
			packet.address = host_physical_address;
		}	

		// Snapshot the current time
		SubsecondTime t_temp = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

		UInt32 data_length = 8; // We assume 8 bytes are accessed during the page table walk
		UInt32 offset = packet.address % 64; // Assuming 64-byte cache lines
		
		// Ensure the access doesn't cross cache line boundary
		// If offset + data_length > 64, cap the offset to ensure we stay within the line
		if (offset + data_length > 64) {
			offset = 64 - data_length;  // Move offset back so access fits in cache line
		}

		IntPtr cache_address = ((IntPtr)(packet.address)) & (~((64 - 1)));
		MMUCacheInterface *l1d_cache = memory_manager->getCacheCntlrAt(core->getId(), MemComponent::component_t::L1_DCACHE);

		// Update the elapsed time in the performance model so that we send the request to the cache at the correct time
		// Example: we need to access 4 levels of the page table:
		// 1. Access the first level of the page table at time t_start
		// 2. Access the second level of the page table at time t_start + latency of the first level
		// 3. Access the third level of the page table at time t_start + latency of the first level + latency of the second level
		// ..

		shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, t_start);

		mmu_base_log->debug("Accessing cache with address:", SimLog::hex(packet.address), "at time", t_start.getNS(), "ns");
		HitWhere::where_t hit_where = HitWhere::UNKNOWN;
		{
			// Demand and prefetch page-table walks take the same L1D -> L2 -> LLC
			// path, so latency and hit_where are computed the same way for both.
			// Previously a prefetch walk went straight to L2 through
			// handleMMUPrefetch, which charged no latency and recorded none of
			// the walker statistics, so a prefetched walk cost nothing and was
			// invisible in the counters.  The line is tagged afterwards, below.
			hit_where = l1d_cache->handleMMUCacheAccess(
			packet.eip,
			packet.lock_signal,
			Core::mem_op_t::READ,
			cache_address, offset,
			NULL, data_length,
			packet.modeled,
			packet.count, packet.type, host_translation_latency);
			
			stats.memory_accesses++;
			mmu_base_log->debug("Cache hit where:", HitWhereString(hit_where));
			
			if (hit_where == HitWhere::where_t::L2_OWN)
				walker_stats.L2_accesses++;
				
			if (hit_where == HitWhere::where_t::NUCA_CACHE)
				walker_stats.NUCA_accesses++;

			if (hit_where == HitWhere::where_t::DRAM_LOCAL || hit_where == HitWhere::where_t::DRAM_REMOTE || hit_where == HitWhere::where_t::DRAM){
				dram_accesses_during_last_walk++;
				walker_stats.DRAM_accesses++;

				if (hit_where == HitWhere::where_t::DRAM_LOCAL)
					walker_stats.DRAM_accesses_local++;
				else if (hit_where == HitWhere::where_t::DRAM_REMOTE)
					walker_stats.DRAM_accesses_remote++;

				// Resolve per-NUMA-node DRAM access
				if (m_numa_enabled) {
					MimicOS* os = Sim()->getMimicOS();
					if (os) {
						UInt32 numa_node = os->getNumaNodeForPPN(packet.address >> 12);
						if (numa_node < m_num_numa_nodes)
							walker_stats.DRAM_accesses_per_numa_node[numa_node]++;
					}
				}
			}
			if (hit_where == HitWhere::where_t::L1_OWN)
				walker_stats.L1D_accesses++;				
				

			// Tag the line a prefetch walk just brought into L2, so that a later
			// demand walk hitting it is credited as a prefetch hit.  tagMMUPrefetch
			// declines when the walk hit in L1 or L2, where the prefetch was
			// redundant.
			if (is_prefetch)
			{
				MMUCacheInterface *l2_cache = memory_manager->getCacheCntlrAt(core->getId(), MemComponent::L2_CACHE);
				if (l2_cache)
					l2_cache->tagMMUPrefetch(cache_address, hit_where);
			}
		}

		out_hit_where = hit_where;

		SubsecondTime t_end = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

		// Restore the elapsed time in the performance model
		shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, t_temp);

		// Tag the cache block with the block type (e.g., page table data)
		memory_manager->tagCachesBlockType(packet.address, packet.type);

		mmu_base_log->debug("---- Finished cache access from MMU");

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
    SubsecondTime MemoryManagementUnitBase::calculatePTWCycles(PTWResult ptw_result, bool count, bool modeled, IntPtr eip, Core::lock_signal_t lock, IntPtr original_va, bool instruction, bool is_prefetch)
	{

		accessedAddresses accesses = ptw_result.accesses;

		translationPacket packet;
		packet.eip = eip; 
		packet.instruction = instruction;
		packet.lock_signal = lock;
		packet.modeled = modeled;
		packet.count = count;
		// Use PAGE_TABLE_INSTRUCTION for instruction translations, PAGE_TABLE_DATA for data translations
		packet.type = instruction ? CacheBlockInfo::block_type_t::PAGE_TABLE_INSTRUCTION : CacheBlockInfo::block_type_t::PAGE_TABLE_DATA;

		SubsecondTime latency = SubsecondTime::Zero();

		int levels = 0;
		int tables = 0;

		for (UInt32 i = 0; i < accesses.size(); i++)
		{
			int level = accesses[i].depth;
			int table = accesses[i].table_level;
			if (level > levels)
				levels = level;
			if (table > tables)	
				tables = table;
		}

		SubsecondTime latency_per_table_per_level[tables + 1][levels + 1];
		HitWhere::where_t hit_loc_per_table_per_level[tables + 1][levels + 1];
		
		// Initialize all elements of the 2D arrays
		for (int t = 0; t <= tables; t++) {
			for (int l = 0; l <= levels; l++) {
				latency_per_table_per_level[t][l] = SubsecondTime::Zero();
				hit_loc_per_table_per_level[t][l] = HitWhere::UNKNOWN;
			}
		}

		int correct_table = -1;
		int correct_level = -1;

		/* iterate through the accesses and calculate the latency for each table and level */
		SubsecondTime t_now =  shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

		mmu_base_log->debug("Starting PTW at time:", t_now.getNS(), "ns");
		mmu_base_log->debug("We need to access", accesses.size(), "addresses");

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

		// Generate unique PTW ID for this page table walk
		UInt64 current_ptw_id = ++m_ptw_id_counter;

		for (int level = 0; level < (levels + 1); level++)
		{
			mmu_base_log->debug("We start performing all the accesses for level:", level);
			for (int tab = 0; tab < (tables + 1); tab++)
			{

				for (UInt32 req = 0; req < accesses.size(); req++)
				{
					int current_level = accesses[req].depth;
					int current_table = accesses[req].table_level;
					IntPtr current_address = accesses[req].physical_addr;
					if (current_level == level && current_table == tab)
					{
						HitWhere::where_t temp_hit_where = HitWhere::UNKNOWN;
						
						// Debug: Check for suspicious addresses (offset in cache line > 60)
						UInt64 offset_in_cl = current_address & 63;
						if (offset_in_cl > 60) {
							std::cout << "[PTW_DEBUG] SUSPICIOUS PTW address=0x" << std::hex << current_address 
							          << " offset_in_cacheline=" << std::dec << offset_in_cl 
							          << " level=" << current_level << " table=" << current_table << std::endl;
						}
						
						// If the request is going to be scheduled to the cache hierarchy AFTER the time that the correct translation is available, 
						// we can skip the access, as it will be redundant
						if(t_now+fetch_delay[tab] < correct_access_translation_time)
						{
							packet.address = current_address;
							
                     // Set metadata context for this PTW access
							MetadataInfo ptw_info;
							ptw_info.is_metadata = true;
							ptw_info.ptw_depth = current_level;
							ptw_info.ptw_level = current_table;  // table_level indicates 4KB/2MB/1GB table type
							ptw_info.ptw_id = current_ptw_id;
							ptw_info.table_id = current_table;
							ptw_info.is_pte = accesses[req].is_pte;
							
							MetadataContext::set(core->getId(), ptw_info);
							
							mmu_base_log->debug("Metadata context set for PTW access - address:", SimLog::hex(current_address), "level:", level, "table:", tab, "ptw_id:", current_ptw_id);
							// Propagate is_prefetch.  It was hardcoded false, so a prefetch walk was
							// indistinguishable from a demand walk inside accessCache: the line was
							// never tagged, and the prefetch walker statistics below could never fire.
							latency = accessCache(packet, t_now+fetch_delay[tab], is_prefetch, temp_hit_where);
							
							// Track prefetch-specific walker stats
							if (is_prefetch) {
								if (temp_hit_where == HitWhere::where_t::L1_OWN)
									walker_stats.L1D_accesses_prefetch++;
								else if (temp_hit_where == HitWhere::where_t::L2_OWN)
									walker_stats.L2_accesses_prefetch++;
								else if (temp_hit_where == HitWhere::where_t::NUCA_CACHE)
									walker_stats.NUCA_accesses_prefetch++;
								else if (temp_hit_where == HitWhere::where_t::DRAM_LOCAL || temp_hit_where == HitWhere::where_t::DRAM_REMOTE || temp_hit_where == HitWhere::where_t::DRAM)
									walker_stats.DRAM_accesses_prefetch++;
							}
							
							// Clear context after access
							MetadataContext::clear(core->getId());
							
							// Call logging hook for PTW access (implemented in derived classes)
							IntPtr cache_line_addr = current_address >> 6;  // Cache line granularity
							logPTWCacheAccess(current_ptw_id, original_va >> 12, current_level, current_table,
							                  cache_line_addr, temp_hit_where, latency, accesses[req].is_pte);
						}
						else{
							latency = SubsecondTime::Zero();
						}
						mmu_base_log->debug("Accessed address:", SimLog::hex(current_address), "level:", level, "table:", tab, "latency:", latency.getNS(), "ns");
					
						if (accesses[req].is_pte == true)
						{
							correct_table = accesses[req].table_level;
							correct_level = accesses[req].depth;
							correct_access_translation_time = t_now + fetch_delay[tab] + latency;
							latency_per_table_per_level[current_table][current_level] = latency;
							hit_loc_per_table_per_level[current_table][current_level] = temp_hit_where;
						}
						else if (latency_per_table_per_level[current_table][current_level] < latency && current_level != correct_level)
						{
							latency_per_table_per_level[current_table][current_level] = latency;
							hit_loc_per_table_per_level[current_table][current_level] = temp_hit_where;
						}

					}
				}
				// We need to update the fetch delay for the next level 
				fetch_delay[tab] += latency_per_table_per_level[tab][level];
				mmu_base_log->trace("Finished PTW for table:", tab, "level:", level, "at time:", (t_now+fetch_delay[tab]).getNS(), "ns");
			}
		}

		mmu_base_log->debug("Finding latency for correct translation");

		// Walking the correct table leads to the calculation of the total walk latency
		// The requests for the other tables will be sent to the cache hierarchy to model the contention 
		// but they will not be charged in the total walk latency

		SubsecondTime walk_latency = SubsecondTime::Zero();

		if (correct_table != -1)
		{
			int cnt_l1 = 0;
            int cnt_l2 = 0;
            int cnt_nuca = 0;
            int cnt_dram = 0;

			mmu_base_log->debug("Found correct translation - table:", correct_table, "level:", correct_level);
			for (int level = 0; level < (levels + 1); level++)
			{

				walk_latency+=latency_per_table_per_level[correct_table][level];

				HitWhere::where_t loc = hit_loc_per_table_per_level[correct_table][level];

				if (loc == HitWhere::where_t::L1_OWN) cnt_l1++;
                else if (loc == HitWhere::where_t::L2_OWN) cnt_l2++;
                else if (loc == HitWhere::where_t::NUCA_CACHE) cnt_nuca++;
                else if (loc == HitWhere::where_t::DRAM_LOCAL) cnt_dram++;

				mmu_base_log->trace("Adding latency for level:", level, "table:", correct_table, "latency:", latency_per_table_per_level[correct_table][level].getNS(), "ns, total:", walk_latency.getNS(), "ns");
			}

			#if ENABLE_MMU_CSV_LOGS
				if (ptw_dump_file.is_open()) {
					ptw_dump_file << (original_va >> 12) << ","
								<< "0x" << std::hex << eip << std::dec << ","
								<< tlb_hits_since_last_ptw << ","
								<< cnt_l1 << ","
								<< cnt_l2 << ","
								<< cnt_nuca << ","
								<< cnt_dram << std::endl;
					tlb_hits_since_last_ptw = 0;  // Reset after each PTW
				}
			#endif
		}
		else {
			mmu_base_log->debug("No correct translation found - using max latency from slowest table");
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
	 * @return PTWOutcome containing the walk latency, fault status, ppn, and page size.
	 */
	PTWOutcome MemoryManagementUnitBase::performPTW(IntPtr address, bool modeled, bool count, bool is_prefetch, IntPtr eip, Core::lock_signal_t lock, PageTable *page_table, bool restart_walk, bool instruction)
	{
			mmu_base_log->section("Starting PTW");
			mmu_base_log->debug("PTW for address:", SimLog::hex(address));

			auto ptw_result = page_table->initializeWalk(address, count, is_prefetch, restart_walk);

			// We will filter out the re-walked addresses which anyways either hit in the PWC or are redundant
			accessedAddresses visited_pts = ptw_result.accesses;
			std::sort(visited_pts.begin(), visited_pts.end());
			visited_pts.erase(std::unique(visited_pts.begin(), visited_pts.end()), visited_pts.end());

			ptw_result.accesses = visited_pts;

			// Filter the PTW result based on the page table type
			// This filtering is necessary to remove any redundant accesses that may hit in the PWC


			mmu_base_log->debug("Accessed", ptw_result.accesses.size(), "addresses");
			if (mmu_base_log->isEnabled(SimLog::LEVEL_TRACE)) {
				visited_pts = ptw_result.accesses;
				for (UInt32 i = 0; i < visited_pts.size(); i++)
				{
					mmu_base_log->trace("Address:", SimLog::hex(visited_pts[i].physical_addr), "Level:", visited_pts[i].depth, "Table:", visited_pts[i].table_level, "Correct:", visited_pts[i].is_pte);
				}
			}

			int page_size = ptw_result.page_size;
			IntPtr ppn_result = ptw_result.ppn;
			bool is_pagefault = ptw_result.fault_happened;
			int requested_frames = ptw_result.requested_frames;

			// Read shadow PTE payload for temporal prefetching
			PayloadWord leaf_payload_bits;
			if (!is_pagefault) {
				uint64_t vpn = address >> 12;
				leaf_payload_bits = page_table->readPayloadBits(vpn);
			}

			auto* mimicos = Sim()->getMimicOS();
			core_id_t pf_core_id = getCore()->getId();
			mimicos->setIsPageFault(pf_core_id, is_pagefault); //  propagate down the call stack that page fault occurred, so that we can mark the instruction to be replayed in trace_thread.cc/run()
			mimicos->setNumRequestedFrames(pf_core_id, requested_frames); // Propagate requested frames for backward compatibility
			
			mmu_base_log->trace("PTW result - is_pagefault:", (is_pagefault ? "True" : "False"));

			if(is_pagefault){

				mimicos->setVaTriggeredPageFault(pf_core_id, address); // Store the virtual address that caused the page fault
				mmu_base_log->debug("Page fault for address:", SimLog::hex(address));
				mmu_base_log->debug("Requested frames:", requested_frames);
			}

			SubsecondTime ptw_cycles = SubsecondTime::Zero();


			// Page fault comes: we do not want to charge the PTW latency for the page fault
			// BUT if we do not charge the PTW latency, we should also NOT insert entries in the PWCs
			// During the "restarted walk", all latencies/locality aspects (hits/misses in PWCs) will be taken into account

			if(is_pagefault && count_page_fault_latency_enabled)
			{
				if (nested_mmu == nullptr)
				{
					ptw_result = filterPTWResult(address, ptw_result, page_table, count);
				}
				ptw_cycles = calculatePTWCycles(ptw_result, count, modeled, eip, lock, address, instruction, is_prefetch);
			}
			// The restarted walk will come under this else-if because the page fault will have been already satisfied
			else if (!is_pagefault)
			{
				
				stats.memory_accesses_before_filtering += ptw_result.accesses.size();

				if (nested_mmu == nullptr)
				{
					ptw_result = filterPTWResult(address, ptw_result, page_table, count);
				}
				ptw_cycles = calculatePTWCycles(ptw_result, count, modeled, eip, lock, address, instruction, is_prefetch);
			}	
			

			
			mmu_base_log->debug("Finished PTW for address: ", address);
			mmu_base_log->debug("PTW latency: ", ptw_cycles);
			mmu_base_log->debug("Physical Page Number: ", ppn_result);
			mmu_base_log->debug("Page Size: ", page_size);
			mmu_base_log->debug("-------------- End of PTW");

			return PTWOutcome(ptw_cycles, is_pagefault, ppn_result, page_size, requested_frames, leaf_payload_bits);
	}

	/**
	 * @brief Perform translation without any timing overhead
	 * 
	 * This is used for perfect_translation mode where we want the PA remapping
	 * benefit (from the allocator) but with zero latency. It:
	 * - Queries the page table for the translation
	 * - Handles page faults if necessary (allocates pages on first access)
	 * - Does NOT charge any TLB or PTW latencies
	 * - Does NOT populate TLBs or PWCs
	 * 
	 * @param address Virtual address to translate
	 * @param page_table Pointer to the page table
	 * @return std::pair<IntPtr, int> containing (physical_address, page_size_bits)
	 */
	std::pair<IntPtr, int> MemoryManagementUnitBase::translateWithoutTiming(IntPtr address, PageTable *page_table)
	{
		mmu_base_log->debug("Perfect translation (zero latency) for address:", SimLog::hex(address));

		// Perform the page table walk - this handles page faults and returns PPN
		// We use restart_walk_after_fault=true so faults are handled automatically
		// count=false and is_prefetch=false since we don't want to count this
		PTWResult ptw_result = page_table->initializeWalk(address, false /* count */, false /* is_prefetch */, true /* restart_walk_after_fault */);

		IntPtr ppn = ptw_result.ppn;
		int page_size = ptw_result.page_size;

		// Calculate physical address: PPN is at 4KB granularity
		constexpr IntPtr base_page_size = 4096;  // 4KB
		IntPtr page_offset = address & ((1ULL << page_size) - 1);
		IntPtr physical_address = (ppn * base_page_size) + page_offset;

		mmu_base_log->debug("Perfect translation result: PA=", SimLog::hex(physical_address), 
		                    " PPN=", ppn, " page_size=", page_size);

		return std::make_pair(physical_address, page_size);
	}

}
