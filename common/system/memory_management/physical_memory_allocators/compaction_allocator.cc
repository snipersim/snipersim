// #include "compaction_thp_allocator.h"
// #include "physical_memory_allocator.h"
// #include <utility>
// #include <cmath>
// #include <iostream>
// #include <fstream>
// #include <list>
// #include <vector>

// #include <algorithm>
// #include <cmath>
// #include <random>
// #include <tuple>
// #include <cassert>
// #include "register_metrics.h"
// #include "globals.h"

// #define DEBUG_RESERVATION_THP

// using namespace std;

// CompactionTHPAllocator::CompactionTHPAllocator(int memory_size, int max_order, int kernel_size, string frag_type, double _threshold_for_promotion) :
// 	PhysicalMemoryAllocator(),
//     threshold_for_promotion(_threshold_for_promotion)
// {
// 	std::cout << "[VirtuOS] Creating Reservation-based THP Allocator" << std::endl;

// 	// initialize the stats 

// 	stats.fragmentation = new std::vector<float>();
// 	stats.two_mb_util_ratio = new std::vector<float>();
// 	stats.four_kb_allocated = 0;
// 	stats.two_mb_reserved = 0;
// 	stats.two_mb_promoted = 0;
// 	stats.two_mb_demoted = 0;

// 	buddy_allocator = new Buddy(memory_size, max_order, kernel_size, frag_type);

// }

// CompactionTHPAllocator::~CompactionTHPAllocator()
// {
// 	delete buddy_allocator;
// }

// bool CompactionTHPAllocator::demote_page()
// {
// 	return false;
// }


// // Create a function that works as a thread that every N seconds checks the fragmentation of the system
// // If the fragmentation is above a certain threshold, we need to defragment to create more 2MB pages

// UInt64 CompactionTHPAllocator::allocate(UInt64 size, UInt64 address, UInt64 core_id)
// {

// 	// // Aggressive THP allocation -> if there is a free zeroed 2MB page, allocate it
// 	// // If there is a free 2MB page which is not zeroed, we will zero it and allocate it


 	
// 	// // This function needs to request the page from the buddy allocator -> searching for order of 9 or bigger
// 	// auto result = search_for_free_2mb_page(); 

// 	// if (!result.first) //First is the address, second is the flag
// 	// {
// 	// 	return givePageFast(size, result.first, core_id);	
// 	// }
	
// 	// if (!result.second){
// 	// 		// We found a free 2MB page
// 	// 		// We need to zero it  if result.second is false

			

// 	// 		// Zero the page
// 	// 		//std::cout << "Zeroing 2MB page" << std::endl;
// 	// 		zero_page(result.first);
			
// 	// }


// 	// update_the_page_table();
// 	// return result.first; 


// }





// UInt64 CompactionTHPAllocator::givePageFast(UInt64 bytes, UInt64 address, UInt64 core_id)
// {
// 	// Normally get the page from the buddy allocator
// 	return 0;
// }

// void CompactionTHPAllocator::deallocate(UInt64 region_begin)
// {
	
// }


// UInt64 CompactionTHPAllocator::getFreePages() const
// {
// 	return buddy_allocator->getFreePages();
// }

// UInt64 CompactionTHPAllocator::getTotalPages() const
// {
// 	return buddy_allocator->getTotalPages();
// }


// double CompactionTHPAllocator::getLargePageRatio()
// {
// 	return buddy_allocator->getLargePageRatio();
// }

// double CompactionTHPAllocator::getAverageSizeRatio()
// {
// 	return buddy_allocator->getAverageSizeRatio();
// }

// void CompactionTHPAllocator::fragmentMemory(double fragmentation)
// {
// 	buddy_allocator->fragmentMemory(fragmentation);
// }