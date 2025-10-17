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

#include "stats.h"
#include "eager_paging.h"
#include "physical_memory_allocator.h"
#include "buddy_allocator.h"
#include "vma.h"
#include "simulator.h"
#include "config.hpp"

//#define DEBUG_EAGER_PAGING

using namespace std;

EagerPagingAllocator::EagerPagingAllocator(String name, int memory_size, int max_order, int kernel_size, String frag_type):
	PhysicalMemoryAllocator(name, memory_size, kernel_size)
{
	log_file_name = "eager_paging.log";
	log_file_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + log_file_name;
	log_file.open(log_file_name);

	std::cout << "[MimicOS] Creating Eager Paging Allocator" << std::endl;

    // Create the buddy allocator
	buddy_allocator = new Buddy(memory_size, max_order, kernel_size, frag_type);

    registerStatsMetric(name, 0, "physical_ranges_per_vma", &stats.physical_ranges_per_vma);
    registerStatsMetric(name, 0, "deviation_of_physical_ranges", &stats.deviation_of_physical_ranges);
    registerStatsMetric(name, 0, "total_allocated_vmas", &stats.total_allocated_vmas);
    registerStatsMetric(name, 0, "total_allocated_physical_ranges", &stats.total_allocated_physical_ranges);
    
}

EagerPagingAllocator::~EagerPagingAllocator()
{
	delete buddy_allocator;
}



std::pair<UInt64, UInt64> EagerPagingAllocator::allocate(UInt64 size, UInt64 address, UInt64 core_id, bool is_pagetable_allocation)
{
	// print at std out the address and size when #define DEBUG_EAGER_PAGING is set

#ifdef DEBUG_EAGER_PAGING
	std::cout << "EagerPagingAllocator::allocate(" << size << ", " << address << ", " << core_id << ")" << std::endl;
#endif
	

}


UInt64 EagerPagingAllocator::givePageFast(UInt64 bytes, UInt64 address, UInt64 core_id)
{
	// print at std out the address and size when #define DEBUG_EAGER_PAGING is set

#ifdef DEBUG_EAGER_PAGING
	std::cout << "EagerPagingAllocator::givePageFast(" << bytes << ", " << address << ", " << core_id << ")" << std::endl;
#endif

	return buddy_allocator->allocate(bytes, address, core_id);
}

void EagerPagingAllocator::deallocate(UInt64 region_begin, UInt64 core_id)
{
	/* Requires implementation */
	
}

std::vector<Range> EagerPagingAllocator::allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id)
{
    /* Requires implementation */

	int remaining_pages = (end_va - start_va)/4096;
	std::vector<Range> ranges;

#ifdef DEBUG_EAGER_PAGING
	log_file << "EagerPagingAllocator::allocate_ranges(" << start_va << ", " << end_va << ", " << app_id << ")" << std::endl;
	log_file << "Start VPN: " << start_va/4096 << std::endl;
	log_file << "End VPN: " << end_va/4096 << std::endl;
	log_file << "Remaining size: " << (end_va - start_va)/4096 << " pages" << std::endl;
#endif
	IntPtr current_vpn = start_va/4096;


	while (remaining_pages > 0)
	{
		//Find the largest power of 2 that is less than the remaining size
		int order = std::log2(remaining_pages);
#ifdef DEBUG_EAGER_PAGING
		log_file << "Order: " << order << std::endl;
#endif
		//Allocate the largest power of 2 that is less than the remaining size

		int requested_size = std::pow(2, order)*4096;
#ifdef DEBUG_EAGER_PAGING
		log_file << "Requested size: " << requested_size << std::endl;
#endif
		std::pair<IntPtr,int>  allocated_range = buddy_allocator->allocate_contiguous(requested_size, app_id);
#ifdef DEBUG_EAGER_PAGING
		log_file << "Allocated range: " << allocated_range.first << " - " << allocated_range.first + allocated_range.second << std::endl;
#endif
		remaining_pages -= allocated_range.second;
#ifdef DEBUG_EAGER_PAGING
		log_file << "Remaining pages: " << remaining_pages << std::endl;
#endif
		Range range;
		range.vpn = current_vpn;
		range.bounds = allocated_range.second;
		range.offset = allocated_range.first;

#ifdef DEBUG_EAGER_PAGING
		log_file << "Pushed Range: " << range.vpn << " - " << range.bounds << " - " << range.offset << std::endl;
#endif

		ranges.push_back(range);
		current_vpn += allocated_range.second;
	}

	return ranges;
    
}


UInt64 EagerPagingAllocator::getFreePages() const
{
	return buddy_allocator->getFreePages();
}

UInt64 EagerPagingAllocator::getTotalPages() const
{
	return buddy_allocator->getTotalPages();
}


double EagerPagingAllocator::getLargePageRatio()
{
	return buddy_allocator->getLargePageRatio();
}

double EagerPagingAllocator::getAverageSizeRatio()
{
	return buddy_allocator->getAverageSizeRatio();
}

void EagerPagingAllocator::fragment_memory()
{
	
	buddy_allocator->fragmentMemory(Sim()->getCfg()->getFloat("perf_model/" + m_name + "/target_fragmentation"));
	return;
}