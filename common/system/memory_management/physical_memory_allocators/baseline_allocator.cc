#include "baseline_allocator.h"
#include "physical_memory_allocator.h"
#include "simulator.h"
#include "config.hpp"

#include <utility>
#include <cmath>
#include <iostream>
#include <fstream>
#include <list>
#include <vector>
#include <algorithm>
#include <cmath>
#include <random>
#include <tuple>
#include <cassert>

//#define DEBUG

using namespace std;

BaselineAllocator::BaselineAllocator(String name, int memory_size, int max_order, int kernel_size, String frag_type) :
	PhysicalMemoryAllocator(name, memory_size, kernel_size)

{
	log_file_name = "baseline_allocator.log";
	log_file_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + log_file_name;
	log_file.open(log_file_name.c_str(), ios::out | ios::app);
	
	
	buddy_allocator = new Buddy(memory_size, max_order, kernel_size, frag_type);

}

/**
 * @brief Allocates a block of physical memory.
 *
 * This function allocates a block of physical memory of the specified size.
 *
 * @param bytes The number of bytes to allocate.
 * @param address The address hint for the allocation.
 * @param core_id The ID of the core requesting the allocation.
 * @param is_pagetable_allocation A flag indicating if the allocation is for a page table.
 * @return The physical address of the allocated memory block.
 */
std::pair<UInt64,UInt64> BaselineAllocator::allocate(UInt64 bytes, UInt64 address, UInt64 core_id, bool is_pagetable_allocation)
{
#ifdef DEBUG
	log_file << "BaselineAllocator::allocate(" << bytes << ", " << address << ", " << core_id << ", " << is_pagetable_allocation << ")" << endl;
#endif
	// If the allocation is for a page table, handle it separately
	if (is_pagetable_allocation)
	{
		UInt64 physical_page = handle_page_table_allocations(bytes);
		return make_pair(physical_page, 12); // Return the physical address and the page size
	}
	else
	{
		// Simply allocate the memory block using the buddy allocator
		UInt64 physical_page = buddy_allocator->allocate(bytes, address, core_id);
		return make_pair(physical_page, 12); // Return the physical address and the page size
	}

}


UInt64 BaselineAllocator::givePageFast(UInt64 bytes, UInt64 address, UInt64 core_id)
{
	// Just for the sake of testing the buddy allocator
	UInt64 physical_page = buddy_allocator->allocate(bytes, address, core_id);
	return physical_page;
}

void BaselineAllocator::deallocate(UInt64 region_begin, UInt64 core_id)
{
}

std::vector<Range> BaselineAllocator::allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id)
{
	// Not implemented - just return an empty vector
	std::vector<Range> ranges;
	return ranges;
}

void BaselineAllocator::fragment_memory()
{
	buddy_allocator->fragmentMemory(Sim()->getCfg()->getFloat("perf_model/" + m_name + "/target_fragmentation"));
	return;
}



