#include "reserve_thp.h"
#include "physical_memory_allocator.h"
#include "stats.h"
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

//#define DEBUG_RESERVATION_THP

using namespace std;

/*
 * ReservationTHPAllocator is a specialized allocator that tries to reserve 2MB pages 
 * whenever possible (i.e., for Transparent Huge Pages or THP). If 2MB reservations fail, 
 * it falls back to a buddy allocator for 4KB pages. This approach allows mixing large pages 
 * (2MB) for contiguous memory when utilization is high enough, and smaller 4KB pages otherwise.
 *
 * Key data structures:
 *   - two_mb_map: Maps a 2MB-region index (region_2MB) to a tuple of:
 *       (1) The starting physical address of that 2MB region,
 *       (2) A bitset<512> representing which 4KB pages within the 2MB region are in use,
 *       (3) A bool flag indicating whether the region has been promoted (fully used as a 2MB page).
 *   - buddy_allocator: A fallback buddy allocator for normal 4KB allocations and for reserving 
 *                      contiguous 2MB blocks when possible.
 *
 * Statistics tracked include:
 *   - four_kb_allocated
 *   - two_mb_reserved
 *   - two_mb_promoted
 *   - two_mb_demoted
 *   - total_allocations
 *   - kernel_pages_used
 *
 * threshold_for_promotion sets the fraction of used 4KB pages in a 2MB region needed to "promote"
 * the entire region to a single 2MB large page mapping.
 */
ReservationTHPAllocator::ReservationTHPAllocator(String name,
                                                 int memory_size,
                                                 int max_order,
                                                 int kernel_size,
                                                 String frag_type,
                                                 float _threshold_for_promotion)
   : PhysicalMemoryAllocator(name, memory_size, kernel_size),
     threshold_for_promotion(_threshold_for_promotion)
{
	log_file_name = "reservation_thp.log";
	log_file_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + log_file_name;
	log_file.open(log_file_name);

	log_file << "[MimicOS] Creating Reservation-based THP Allocator" << std::endl;

	// Initialize the stats counters
	stats.four_kb_allocated = 0;
	stats.two_mb_reserved   = 0;
	stats.two_mb_promoted   = 0;
	stats.two_mb_demoted    = 0;
	stats.total_allocations = 0;
	stats.kernel_pages_used = 0;

	// Create a buddy allocator for fallback memory requests
	buddy_allocator = new Buddy(memory_size, max_order, kernel_size, frag_type);

	// Register stats metrics
	registerStatsMetric(name, 0, "four_kb_allocated", &stats.four_kb_allocated);
	registerStatsMetric(name, 0, "two_mb_reserved", &stats.two_mb_reserved);
	registerStatsMetric(name, 0, "two_mb_promoted", &stats.two_mb_promoted);
	registerStatsMetric(name, 0, "two_mb_demoted", &stats.two_mb_demoted);
	registerStatsMetric(name, 0, "total_allocations", &stats.total_allocations);
	registerStatsMetric(name, 0, "kernel_pages_used", &stats.kernel_pages_used);
}

ReservationTHPAllocator::~ReservationTHPAllocator()
{
	delete buddy_allocator;
}

/*
 * demote_page():
 *   - Searches through all allocated 2MB regions in two_mb_map to find the one with the
 *     lowest utilization (least allocated 4KB pages). That region is then "demoted" by 
 *     freeing its unused pages, removing the region from two_mb_map, and letting the 
 *     buddy allocator take back the partially unused space.
 *   - This can free up memory for new allocations. Typically used when no large or small
 *     allocations can succeed, so we demote a region to reclaim partial space.
 *
 * Returns 'true' if a demotion occurred, or 'false' if there are no demotable 2MB regions.
 */
bool ReservationTHPAllocator::demote_page()
{
	// We'll collect <2MB_index, utilization> pairs and sort them by ascending utilization
	std::vector<std::pair<UInt64, double>> utilization;

#ifdef DEBUG_RESERVATION_THP
	log_file << "Debug: Starting to sort region_2MB based on utilization" << std::endl;
#endif

	for (auto it = two_mb_map.begin(); it != two_mb_map.end(); it++)
	{
		// Skip any regions already "promoted" (fully used or mapped at 2MB)
		if (std::get<2>(it->second))
			continue;

		double util = static_cast<double>(std::get<1>(it->second).count()) / 512;
		utilization.push_back(std::make_pair(it->first, util));
	}

	// Sort by ascending utilization
	std::sort(utilization.begin(), utilization.end(),
	          [](std::pair<UInt64, double> &left, std::pair<UInt64, double> &right)
	          { return left.second < right.second; });

#ifdef DEBUG_RESERVATION_THP
	log_file << "Debug: Sorted utilization vector" << std::endl;
#endif

	// If there's no 2MB region to demote, return false
	if (utilization.size() == 0)
	{
#ifdef DEBUG_RESERVATION_THP
		log_file << "Debug: No regions to demote, returning false" << std::endl;
#endif
		return false;
	}

	// Take the region with the lowest utilization
	UInt64 region_2MB = utilization[0].first;
	UInt64 region_begin = std::get<0>(two_mb_map[region_2MB]);
	UInt64 region_size  = 512; // 512 pages of 4KB each => 2MB total
	UInt64 chunk        = region_size;

#ifdef DEBUG_RESERVATION_THP
	log_file << "Debug: Removing region_2MB with utilization: " << utilization[0].second << std::endl;
#endif

	// For each 4KB sub-page not in use (bit is not set), free it in the buddy allocator
	for (UInt64 j = 0; j < chunk; j++)
	{
		if (std::get<1>(two_mb_map[region_2MB])[j])
			continue;

		buddy_allocator->free(region_begin + j, region_begin + (j + 1));
	}

#ifdef DEBUG_RESERVATION_THP
	log_file << "Demoted 2MB region with utilization: " << utilization[0].second
	         << " and freed " << 512 - std::get<1>(two_mb_map[region_2MB]).count() << " pages" << std::endl;
#endif

	stats.two_mb_demoted++;

	// Remove this region from the map entirely
	two_mb_map.erase(utilization[0].first);

#ifdef DEBUG_RESERVATION_THP
	log_file << "Debug: Updated stats and erased region_2MB from two_mb_map" << std::endl;
#endif

	return true;
}

/*
 * checkFor2MBAllocation(...):
 *   - Given a virtual address, determines if the 4KB page can be allocated as part of a 2MB region.
 *   - The region index is region_2MB = address >> 21 (for 2MB alignment).
 *   - If not yet reserved, tries to reserve via buddy_allocator->reserve_2mb_page().
 *   - If successful, records it in two_mb_map[region_2MB] = (region_begin, bitset<512>, not_promoted).
 *   - Then sets the relevant bit (offset_in_2MB) to mark the 4KB page as allocated.
 *   - If the fraction of allocated sub-pages > threshold_for_promotion, mark the region as "promoted."
 *
 * Returns:
 *   ( physical_address_of_4KBpage_within_2MB , bool indicating if just promoted )
 * Or (-1, false) if no reservation could be made.
 */


std::pair<UInt64, bool> ReservationTHPAllocator::checkFor2MBAllocation(UInt64 address, UInt64 core_id)
{
	UInt64 region_2MB = address >> 21;           // ID for which 2MB chunk in the VA space
	// offset_in_2MB is the 4KB-page index within that chunk
	// We'll recalc it if we do find or create a region

#ifdef DEBUG_RESERVATION_THP
	log_file << "Debug: Calculated region_2MB = " << region_2MB << std::endl;
#endif

	// If we have not yet reserved that 2MB region, try to do so now
	if (two_mb_map.find(region_2MB) == two_mb_map.end())
	{
#ifdef DEBUG_RESERVATION_THP
		log_file << "Debug: region_2MB not found in two_mb_map" << std::endl;
#endif
		auto two_mb_reserved_region = buddy_allocator->reserve_2mb_page(address, core_id);
#ifdef DEBUG_RESERVATION_THP
		log_file << "Debug: Called reserve_2mb_page, result = " << std::get<0>(two_mb_reserved_region) << std::endl;
#endif
		// If we couldn't reserve 2MB, return -1
		if (std::get<0>(two_mb_reserved_region) == static_cast<UInt64>(-1))
		{
#ifdef DEBUG_RESERVATION_THP
			log_file << "Debug: two_mb_reserved_region is nullptr, returning -1" << std::endl;
#endif
			return std::make_pair((UInt64)-1, false);
		}
		else
		{
			// We successfully reserved a 2MB chunk, create a new entry in two_mb_map
			two_mb_map[region_2MB] = std::make_tuple(std::get<0>(two_mb_reserved_region),
			                                         std::bitset<512>(), 
			                                         false /* not promoted*/ );
			stats.two_mb_reserved++;
#ifdef DEBUG
			log_file << "Debug: Reserved 2MB region, updated two_mb_map and stats.two_mb_reserved = "
			         << stats.two_mb_reserved << std::endl;
#endif
		}
	}

	// Retrieve the region info from two_mb_map
	auto &region = two_mb_map[region_2MB];

#ifdef DEBUG_RESERVATION_THP
	log_file << "Debug: Retrieved region from two_mb_map" << std::endl;
#endif

	// If region has already been "promoted," we logically shouldn't have a page fault here
	if (std::get<2>(region))
	{
#ifdef DEBUG_RESERVATION_THP
		log_file << "Debug: Page is already promoted" << std::endl;
#endif
		// If the page was promoted, we wouldn't typically be calling this. So it’s an assert.
		assert(false);
	}
	else
	{
		// Mark the correct offset within the 2MB region as in use
		auto &bitset = std::get<1>(region);
		int offset_in_2MB = (address >> 12) & 0x1FF;  // local re-calc

#ifdef DEBUG_RESERVATION_THP
		log_file << "Debug: Page is not promoted, recalculated offset_in_2MB = "
		         << offset_in_2MB << std::endl;
#endif

		bitset.set(offset_in_2MB);

#ifdef DEBUG_RESERVATION_THP
		log_file << "Debug: Marked page as used in bitset: ";
		for (int i = 0; i < 512; i++)
		{
			log_file << bitset[i];
		}
		log_file << std::endl;
#endif

/* @kanellok: FOCA -  The utilization fraction is calculated as the number of used 4KB pages in 
 * the 2MB region divided by 512 (the total number of 4KB pages in a 2MB region).
 * If the utilization exceeds the threshold_for_promotion, we "promote" the entire region
 * to a huge page, meaning we treat the entire 2MB region as a single large page.
 * This is done by setting the third element of the tuple in two_mb_map to true
 * and returning the physical address of the 4KB page within that region.
 * If the region is not promoted, we simply return the physical address of the 4KB page
 * within the 2MB region without promoting it.
 */

 /* @kanellok:
 * FOCA: You can add your promotion logic here including the new features
 */
		// Compute the utilization fraction for this region
		float utilization = static_cast<float>(bitset.count()) / 512;

#ifdef DEBUG_RESERVATION_THP
		log_file << "Debug: Calculated utilization = " << utilization
		         << " with threshold_for_promotion = "
		         << threshold_for_promotion << std::endl;
#endif
		// If utilization exceeds the threshold, we "promote" the entire region as a huge page
		bool ready_to_promote = (utilization > threshold_for_promotion);
		if (ready_to_promote && !std::get<2>(region))
		{
			std::get<2>(region) = true;  // Mark as promoted
			stats.two_mb_promoted++;

#ifdef DEBUG_RESERVATION_THP
			log_file << "Debug: Promoted page, updated stats.two_mb_promoted = "
			         << stats.two_mb_promoted << std::endl;
#endif
			// Return the physical address for the offset_in_2MB, but note that we "just promoted"
			return std::make_pair(std::get<0>(region) + offset_in_2MB, true);
		}
		else
		{
			// If not promoted, just return the 4KB offset within the 2MB region
#ifdef DEBUG_RESERVATION_THP
			log_file << "Debug: Page is not promoted, returning physical address "
			         << std::get<0>(region) + offset_in_2MB  << std::endl;
#endif
			return std::make_pair(std::get<0>(region) + offset_in_2MB, false);
		}
	}

	// Should not reach here
	return std::make_pair((UInt64)-1, false);
}

/**
 * allocate(...):
 *   - This is the main entry point for user-level (non-pagetable) allocations.
 *   - If is_pagetable_allocation == true, we pass it on to the buddy allocator 
 *     because page tables are always 4KB in this model.
 *   - Otherwise, we check if we can place the address in a 2MB region via checkFor2MBAllocation(...).
 *     * If that returns a valid address, we use it. Possibly at 21 bits if "promoted," or 12 bits 
 *       if not.
 *     * If 2MB fails, we fallback to the buddy allocator for a 4KB allocation.
 *       - If the buddy also fails, we try demote_page() to free partial 2MB regions, 
 *         and then retry. 
 *
 * Returns:
 *   ( physical_address , page_size_in_bits ) or ( -1 , 12 ) if no memory is available.
 */

 /* @kanellok: FOCA -  This function is the core of the THP allocator. It handles both 2MB and 4KB allocations.
 */
std::pair<UInt64, UInt64> ReservationTHPAllocator::allocate(UInt64 size, UInt64 address, UInt64 core_id, bool is_pagetable_allocation)
{
	stats.total_allocations++;
#ifdef DEBUG_RESERVATION_THP
	log_file << "ReservationTHPAllocator::allocate(" << size << ", " << address << ", " << core_id << ")" << std::endl;
#endif

	// Page table allocations always go to the buddy allocator in 4KB form
	if (is_pagetable_allocation)
	{
		auto page = buddy_allocator->allocate(size, address, core_id);
#ifdef DEBUG_RESERVATION_THP
		log_file << "Debug: Pagetable allocation, result = " << page << std::endl;
#endif
		stats.four_kb_allocated++;
		return make_pair(page, 12);
	}

	// Attempt to allocate within a 2MB chunk
	auto page_2mb_result = checkFor2MBAllocation(address, core_id);

#ifdef DEBUG_RESERVATION_THP
	log_file << "Debug: Checked for 2MB allocation, result = " << page_2mb_result.first << std::endl;
#endif

	// If we got a valid address, we either just reserved or used an existing 2MB region
	if (page_2mb_result.first != static_cast<UInt64>(-1))
	{
#ifdef DEBUG_RESERVATION_THP
		log_file << "Debug: There is a 2MB allocation (either promoted now or reserved now), returning physical address" << std::endl;
#endif
		if (page_2mb_result.second)
		{
			// The page got promoted to 2MB
#ifdef DEBUG_RESERVATION_THP
			log_file << "Debug: The page just got promoted, returning physical address with 2MB flag" << std::endl;
#endif
			return make_pair(page_2mb_result.first, 21);
		}
		else
		{
			// We remain in 4KB mode inside that 2MB region
#ifdef DEBUG_RESERVATION_THP
			log_file << "Debug: The page is not promoted, returning physical address with 4KB flag" << std::endl;
#endif
			return make_pair(page_2mb_result.first, 12);
		}
	}
	else
	{
		// 2MB allocation did not succeed; fallback to buddy
#ifdef DEBUG_RESERVATION_THP
		log_file << "Debug: No 2MB allocation, falling back to buddy allocator" << std::endl;
#endif
		auto page_fallback = buddy_allocator->allocate(size, address, core_id);

		// If buddy works, we get a 4KB allocation
		if (page_fallback != static_cast<UInt64>(-1))
		{
#ifdef DEBUG_RESERVATION_THP
			log_file << "Debug: Buddy allocator succeeded, returning physical address with 4KB flag" << std::endl;
#endif
			stats.four_kb_allocated++;
			return make_pair(page_fallback, 12);
		}
		else
		{
			// If buddy fails, try demoting a partially used 2MB region
#ifdef DEBUG_RESERVATION_THP
			log_file << "Debug: Buddy allocator failed, attempting to demote a page" << std::endl;
#endif
			bool demoted = demote_page();
			if (demoted)
			{
				stats.four_kb_allocated++;
				auto page_fallback = buddy_allocator->allocate(size, address, core_id);
				return make_pair(page_fallback, 12);
			}
			else
			{
				assert(false);
				// No pages left to demote => out of memory
				return make_pair((UInt64)-1, 12);
			}
		}
	}
}

/*
 * givePageFast(...) => If we don't need the THP approach, we do a direct buddy allocation.
 */
UInt64 ReservationTHPAllocator::givePageFast(UInt64 bytes, UInt64 address, UInt64 core_id)
{
#ifdef DEBUG_RESERVATION_THP
	log_file << "ReservationTHPAllocator::givePageFast(" << bytes << ", " << address << ", " << core_id << ")" << std::endl;
#endif
	return buddy_allocator->allocate(bytes, address, core_id);
}

/*
 * deallocate(...) => Not implemented. Could free 4KB or 2MB pages from the relevant structures.
 */
void ReservationTHPAllocator::deallocate(UInt64 region_begin, UInt64 core_id)
{
	/* Requires implementation */
}

/*
 * allocate_ranges(...) => Not used in this model. Could be extended if needed.
 */
std::vector<Range> ReservationTHPAllocator::allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id)
{
	std::vector<Range> ranges;
	return ranges;
}

/*
 * Stats / Info pass-throughs. Provide info about free pages, total pages, large page ratio, etc.
 */
UInt64 ReservationTHPAllocator::getFreePages() const
{
	return buddy_allocator->getFreePages();
}

UInt64 ReservationTHPAllocator::getTotalPages() const
{
	return buddy_allocator->getTotalPages();
}

double ReservationTHPAllocator::getLargePageRatio()
{
	return buddy_allocator->getLargePageRatio();
}

double ReservationTHPAllocator::getAverageSizeRatio()
{
	return buddy_allocator->getAverageSizeRatio();
}

/*
 * fragment_memory(...) => forcibly fragment memory in the buddy allocator, used for testing.
 */
void ReservationTHPAllocator::fragment_memory()
{
	buddy_allocator->fragmentMemory(Sim()->getCfg()->getFloat("perf_model/" + m_name + "/target_fragmentation"));
	return;
}
