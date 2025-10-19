#include "pagetable_radix.h"
#include "pagetable.h"
#include "simulator.h"
#include "physical_memory_allocator.h"
#include "mimicos.h"

//#define DEBUG

namespace ParametricDramDirectoryMSI
{

	/**
	 * @brief Constructor for the PageTableRadix class.
	 *
	 * @param core_id The core ID.
	 * @param name The name of the page table.
	 * @param page_sizes The number of page sizes.
	 * @param page_size_list The list of page sizes.
	 * @param levels The number of levels in the page table.
	 * @param frame_size The size of each frame.
	 * @param _pwc Pointer to the Page Walk Cache (PWC).
	 * @param is_guest Boolean indicating if this is a guest page table.
	 */

	PageTableRadix::PageTableRadix(int core_id, String name, String type, int page_sizes, int *page_size_list, int levels, int frame_size, bool is_guest)
		: PageTable(core_id, name, type, page_sizes, page_size_list, is_guest),
		  m_frame_size(frame_size),
		  levels(levels)
	{

		log_file = std::ofstream();
		log_file_name = std::string(name.c_str()) + ".pagetable.log." + std::to_string(core_id);
		log_file_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + log_file_name;
		log_file.open(log_file_name.c_str());

		log_file << "[Page Table] Creating Radix-based page table" << std::endl;
		bzero(&stats, sizeof(stats));

		registerStatsMetric(name, core_id, "page_faults", &stats.page_faults);
		registerStatsMetric(name, core_id, "page_table_walks", &stats.page_table_walks);
		registerStatsMetric(name, core_id, "ptw_num_cache_accesses", &stats.ptw_num_cache_accesses);
		registerStatsMetric(name, core_id, "pf_num_cache_accesses", &stats.pf_num_cache_accesses);
		registerStatsMetric(name, core_id, "allocated_frames", &stats.allocated_frames);

		stats.page_size_discovery = new UInt64[m_page_sizes];

		for (int i = 0; i < m_page_sizes; i++)
		{
			stats.page_size_discovery[i] = 0;
			registerStatsMetric(name, core_id, "page_size_discovery_" + itostr(i), &stats.page_size_discovery[i]);
		}

		root = (PTFrame *)malloc(sizeof(PTFrame));
		root->entries = (PTEntry *)malloc(sizeof(PTEntry) * m_frame_size);

		// @hsongara: Get the OS object
		MimicOS* os;
		os = Sim()->getMimicOS();
		root->emulated_ppn = os->getMemoryAllocator()->handle_page_table_allocations(4096);
#ifdef DEBUG
		log_file << "Root frame: " << root << std::endl;
#endif

		for (int i = 0; i < m_frame_size; i++)
		{
			root->entries[i].is_pte = false;
			root->entries[i].data.next_level = NULL;
		}
	}

	/**
	 * @brief Initializes a page table walk for a given address.
	 *
	 * @param address The virtual address to walk.
	 * @param count Boolean indicating if the walk should be counted in the statistics.
	 * @param is_prefetch Boolean indicating if this is a prefetch operation.
	 * @return PTWResult The result of the page table walk.
	 */

	PTWResult PageTableRadix::initializeWalk(IntPtr address, bool count, bool is_prefetch, bool restart_walk_after_fault)
	{

#ifdef DEBUG
		log_file << std::endl;
		log_file << "[RADIX] --------------------------------------------" << std::endl;
		log_file << "[RADIX] RADIX is coming.. with address " << address << std::endl;
#endif

		if (count)
			stats.page_table_walks++;

		// @hsongara: Get the OS object
		MimicOS* os;
		os = Sim()->getMimicOS();

		bool is_pagefault = false;
		AllocatedPage page_fault_result; // Stores the result of the page fault handling IF a page fault occurs

		accessedAddresses visited_pts; // In cases of page faults, we replay the walk but we DO NOT RESET the visited_pts as the
									   // execution time will be dominated by the page fault latency anyways

		// Time --------------------------------------------------------------------------------------------------->
		// L2 TLB Miss -> initializeWalk -> handlePageFault() -> restart_walk (label) -> return PTW Result

	restart_walk: // Get the 9 MSB of the address

		IntPtr offset = (address >> 39) & 0x1FF;

		// Start the walk from the root
		PTFrame *current_frame = root;

#ifdef DEBUG
		log_file << "[RADIX] Accessing PT address: " << address << " at level: " << levels << " with offset: " << offset << std::endl;
#endif

		IntPtr ppn_result;
		IntPtr page_size_result;

		int counter = 0; // Stores the depth of the pointer-chasing
		int i = 0;		 // Stores the page table id

		int level = levels;
		SubsecondTime pwc_latency = SubsecondTime::Zero();

		while (level > 0)
		{
			offset = (address >> (48 - 9 * (levels - level + 1))) & 0x1FF;

#ifdef DEBUG
			log_file << "[RADIX] Accessing PT address: " << current_frame << " at level: " << level << " with offset: " << offset << std::endl;
#endif
			visited_pts.push_back(std::make_tuple(i, counter, (IntPtr)(current_frame->emulated_ppn * 4096 + offset * 8), current_frame->entries[offset].is_pte && current_frame->entries[offset].data.translation.valid));

#ifdef DEBUG
			log_file << "[RADIX] Pushed in visited: " << i << " " << counter << " " << (IntPtr)(current_frame->emulated_ppn * 4096 + offset * 8) << " " << (current_frame->entries[offset].is_pte && current_frame->entries[offset].data.translation.valid) << std::endl;
#endif
			if (current_frame->entries[offset].is_pte)
			{

				// The entry is not valid, we need to handle a page fault
				if (current_frame->entries[offset].data.translation.valid == false)
				{
					if (restart_walk_after_fault)
						os->handle_page_fault(address, core_id, getMaxLevel());

					is_pagefault = true;

					stats.page_faults++;
#ifdef DEBUG
					log_file << "[PAGE FAULT RESOLVED] for address: " << address << std::endl;
					log_file << std::endl;
#endif
					if (restart_walk_after_fault)
						goto restart_walk;
					else
						return PTWResult(page_size_result, visited_pts, ppn_result, pwc_latency, is_pagefault);
				}
				// We found the entry, we can return the result

				if (count)
					stats.page_size_discovery[level - 1]++;
#ifdef DEBUG
				log_file << "[RADIX] Found translation for address: " << address << " with ppn: " << current_frame->entries[offset].data.translation.ppn << " at level: " << level << " with page size: " << m_page_size_list[level - 1] << std::endl;
#endif
				// @kanellok: Be careful with the return values -> always return PPN_RESULT at page size granularity
				ppn_result = current_frame->entries[offset].data.translation.ppn;
				page_size_result = m_page_size_list[level - 1]; // If we hit at level 1 (last one), we return the page_size[1-1] = page_size[0] = 4KB
				break;
			}
			else
			{
#ifdef DEBUG
				log_file << "[RADIX] Moving to the next level" << std::endl;
#endif
				// The entry was a pointer to the next level of the page table
				// We need to chase the pointer -> if the next level is NULL, we need to handle a page fault
				if (current_frame->entries[offset].data.next_level == NULL)
				{
#ifdef DEBUG
					log_file << "[RADIX] Next level is NULL, we need to allocate a new frame" << std::endl;
#endif
					if (restart_walk_after_fault)
						os->handle_page_fault(address, core_id, getMaxLevel());

					stats.page_faults++;
					is_pagefault = true;

#ifdef DEBUG
					log_file << "[RADIX] Page fault resolved for address: " << address << std::endl;
					log_file << "[RADIX] Restarting the walk" << std::endl;
#endif
					if (restart_walk_after_fault)
						goto restart_walk;
					else
						return PTWResult(page_size_result, visited_pts, ppn_result, pwc_latency, is_pagefault);
				}
				else
				{
					current_frame = current_frame->entries[offset].data.next_level;
				}
			}

			// Move to the next level
			// 4->3->2->1
			level--;
			counter++;
		}

#ifdef DEBUG
		log_file << "[RADIX] Finished walk for address: " << address << std::endl;
		log_file << "[RADIX] Final physical page number: " << ppn_result << std::endl;
		log_file << "[RADIX] Final page size: " << page_size_result << std::endl;
		log_file << "[RADIX] --------------------------------------------" << std::endl;
#endif

		return PTWResult(page_size_result, visited_pts, ppn_result, pwc_latency, is_pagefault);
	}

	int PageTableRadix::updatePageTableFrames(IntPtr address, IntPtr core_id, IntPtr ppn, int page_size, std::vector<UInt64> frames)
	{
// #ifdef DEBUG
// 		log_file << "[RADIX] I was provided with the following frames: " << std::endl;
// 		for (int i = 0; i < frames.size(); i++)
// 		{
// 			log_file << "[RADIX] Frame: " << frames[i] << std::endl;
// 		}
// #endif
		PTFrame *current_frame = root;
		PTFrame *previous_frame = NULL;

		IntPtr offset = (address >> 39) & 0x1FF;
		IntPtr previous_offset = static_cast<IntPtr>(-1);

		int level = levels;
		int counter = 0;

#ifdef DEBUG
		log_file << std::endl;
		log_file << "[RADIX] Updating page table frames for address: " << address << " with ppn: " << ppn << " and page size: " << page_size << std::endl;
#endif

		accessedAddresses pagefault_addresses;

		// Walk the page table to the last level and update the page table frames which are not yet allocated
		int frames_used = 0;

		while (level > 0)
		{
			offset = (address >> (48 - 9 * (levels - level + 1))) & 0x1FF;

			// Move to the next level of the page table

#ifdef DEBUG
			log_file << "[RADIX] Accessing: " << current_frame << " at level: " << level << " with offset: " << offset << std::endl;
#endif

			if (current_frame == NULL)
			{
#ifdef DEBUG
				log_file << "[RADIX] Current frame is NULL, we need to allocate a new frame" << std::endl;

#endif

				PTFrame *new_pt_frame = new PTFrame;
				stats.allocated_frames++;

				new_pt_frame->entries = new PTEntry[m_frame_size];
				new_pt_frame->emulated_ppn = frames[frames_used];
				frames_used++;
				current_frame = new_pt_frame;
#ifdef DEBUG
				log_file << "[RADIX] New frame allocated: " << current_frame << " with ppn: " << frames[counter] << std::endl;
				log_file << "[RADIX] Previous frame: " << previous_frame << " at offset" << previous_offset << " is updated with the new frame: " << current_frame << std::endl;

#endif
				previous_frame->entries[previous_offset].data.next_level = current_frame;

				bool is_pte = level == 1 ? true : false;
				for (int i = 0; i < m_frame_size; i++)
				{

					new_pt_frame->entries[i].is_pte = is_pte;
					new_pt_frame->entries[i].data.next_level = NULL;
				}
			}
			else
			{

				// Allocate a new page table
				if (page_size == 21 && level == 2)
				{
#ifdef DEBUG
					log_file << "[RADIX] Let's update the PTE: " << current_frame << " with ppn: " << ppn << " at level: " << level << " with page size: " << page_size << std::endl;
#endif
					current_frame->entries[offset].data.translation.valid = true;
					current_frame->entries[offset].data.translation.ppn = ppn;
					current_frame->entries[offset].is_pte = true;
					break;
				}
				else if (page_size == 12 && level == 1)
				{

#ifdef DEBUG
					log_file << "[RADIX] Let's update the PTE: " << current_frame << " with ppn: " << ppn << " at level: " << level << " with page size: " << page_size << std::endl;
#endif
					current_frame->entries[offset].data.translation.valid = true;
					current_frame->entries[offset].data.translation.ppn = ppn;
					break;
				}

				previous_frame = current_frame;
				current_frame = current_frame->entries[offset].data.next_level;
#ifdef DEBUG
				log_file << "[RADIX] Let's jump to the next level: " << current_frame << std::endl;
#endif
				previous_offset = offset;
				level--;
				counter++;
			}
		}
		return frames_used;
	}

	void PageTableRadix::deletePage(IntPtr address)
	{

#ifdef DEBUG
		log_file << "[RADIX] Deleting page that corresponds to address: " << address << std::endl;
#endif
		PTFrame *current_frame = root;
		IntPtr offset = (address >> 39) & 0x1FF;

		int level = levels;
		int counter = 0;

		while (level > 0)
		{
			offset = (address >> (48 - 9 * (levels - level + 1))) & 0x1FF;

			if (current_frame->entries[offset].is_pte)
			{
#ifdef DEBUG
				log_file << "[RADIX] Found the PTE for address: " << address << " at level: " << level << " with offset: " << offset << std::endl;
#endif
				current_frame->entries[offset].data.translation.valid = false;
				current_frame->entries[offset].data.translation.ppn = -1;
				break;
			}
			else
			{
				// Move to the next level of the page table
				current_frame = current_frame->entries[offset].data.next_level;
			}
			level--;
			counter++;
		}
	}

	/**
	 * @brief Allocates physical space for the page table.
	 *
	 * @param size The size of the physical space to allocate.
	 * @return IntPtr The physical address of the allocated space.
	 */
	IntPtr PageTableRadix::getPhysicalSpace(int size)
	{
		// We directly ask it from the VirtuOS
		MimicOS* os;
		os = Sim()->getMimicOS();
		
		return os->getMemoryAllocator()->handle_page_table_allocations(size);
	}

}