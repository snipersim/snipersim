#pragma once

#include "debug_config.h"
#include "memory_management/physical_memory_allocators/physical_memory_allocator.h"
#include <bitset>
#include <map>
#include <tuple>
#include <vector>
#include <utility>
#include <cassert>

#include "templates_traits_config.h"

// Provides template specialisation for Buddy Type mapping: based on Policy template type used in instantiating ReservationTHPAllocator
#include "memory_management/physical_memory_allocators/buddy_policy_traits.h"
#include "memory_management/physical_memory_allocators/buddy.h"

#include "debug_config.h"

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

/**
 * FragmentationMode:
 *   Enum to specify how memory fragmentation should be controlled.
 *   - RATIO_BASED: Fragment until a target ratio of free 2MB pages is achieved (0.0 to 1.0)
 *   - COUNT_BASED: Fragment until exactly the specified number of free 2MB pages remain
 */
enum class FragmentationMode {
    RATIO_BASED,
    COUNT_BASED
};

 using namespace std;
template <typename Policy>
class ReservationTHPAllocator : public PhysicalMemoryAllocator, private Policy
{
    using BuddyPolicy = typename BuddyPolicyFor<Policy>::type;
    using BuddyType =   Buddy<BuddyPolicy>;

private:
            struct Stats {
                UInt64 four_kb_allocated = 0;
                UInt64 two_mb_reserved   = 0;
                UInt64 two_mb_promoted   = 0;
                UInt64 two_mb_demoted    = 0;
                UInt64 total_allocations = 0;
                UInt64 kernel_pages_used = 0;
            } stats;
            
    FragmentationMode m_frag_mode = FragmentationMode::RATIO_BASED;
    double m_target_frag_ratio = 1.0;           // For RATIO_BASED mode
    UInt64 m_target_free_2mb_count = 0;         // For COUNT_BASED mode
    
public:
    Stats& getStats() { return stats; }

    ReservationTHPAllocator(String name,
                            int memory_size,
                            int max_order,
                            int kernel_size,
                            String frag_type,
                            double threshold_for_promotion)
        : PhysicalMemoryAllocator(name, memory_size, kernel_size),
          threshold_for_promotion(threshold_for_promotion)
    {
        Policy::on_init(name, memory_size, kernel_size, threshold_for_promotion, this);

        // PROBLEM: include‑order contract problem: Right now, reserve_thp.h assumes that BuddyPolicyFor<Policy> is fully specialized. If someone forgets to include the side‑specific buddy_policy.h first, compilation fails with a cryptic "incomplete type" error.
        // SOLUTION (self‑documenting and enforceable at compile‑time): define type trait "is_complete" to validate type is complete
        static_assert(is_complete<BuddyPolicyFor<Policy>>::value,
                        "BuddyPolicyFor<Policy> is incomplete. Did you include the correct buddy_policy.h before reserve_thp.h?");

        buddy_allocator = new BuddyType(memory_size, max_order, kernel_size, frag_type);
    }

    ~ReservationTHPAllocator()
    {
        delete buddy_allocator;
    }

    /**
     * allocate(...):
     *   - This is the main entry point for user-level (non-pagetable) allocations.
     *   - If is_pagetable_allocation == true, we pass it on to the buddy allocator 
     *     because page tables are always 4KB in this model.
     *   - Otherwise (data or instruction), we check if we can place the address in a 2MB region
     *     via checkFor2MBAllocation(...).
     *     * If that returns a valid address, we use it. Possibly at 21 bits if "promoted," or 12 bits 
     *       if not. Instruction pages always remain 4KB even if the region is promoted.
     *     * If 2MB fails, we fallback to the buddy allocator for a 4KB allocation.
     *       - If the buddy also fails, we try demote_page() to free partial 2MB regions, 
     *         and then retry. 
     *
     * Returns:
     *   ( physical_address , page_size_in_bits ) or ( -1 , 12 ) if no memory is available.
     */
    std::pair<UInt64, UInt64> allocate(UInt64 size, UInt64 address = 0, UInt64 core_id = -1, bool is_pagetable_allocation = false, bool /*is_instruction_allocation*/ = false) override
    {
        stats.total_allocations++;
        this->log("allocate: size=", size, "addr=", address, "core=", core_id);

        // Page table allocations always go to the buddy allocator in 4KB form
        if (is_pagetable_allocation)
        {
            auto page = buddy_allocator->allocate(size, address, core_id);
            this->log("Pagetable allocation, result=", page);
            stats.four_kb_allocated++;
            return std::make_pair(page, 12);
        }

        // Instruction allocations follow the same 2MB reservation path as data allocations.
        // This ensures that instruction and data accesses to the same VA page get the same PPN,
        // avoiding VA-PA mapping inconsistencies.

        // Attempt to allocate within a 2MB chunk
        auto page2mb = checkFor2MBAllocation(address, core_id);
        this->log("Checked for 2MB allocation, result=", page2mb.first);

        // If we got a valid address, we either just reserved or used an existing 2MB region
        if (page2mb.first != static_cast<UInt64>(-1)) {
            this->log("2MB allocation found, returning physical address");
            if (page2mb.second) { // The region got promoted to 2MB
                this->log("Page promoted to 2MB");
                return make_pair(page2mb.first, 21);
            }
            else { // We remain in 4KB mode inside that 2MB region
                this->log("Page not promoted, returning 4KB");
                return make_pair(page2mb.first, 12);
            }
        }

        // 2MB allocation did not succeed; fallback to buddy
        this->log("No 2MB allocation, falling back to buddy allocator");
        auto fallback = buddy_allocator->allocate(size, address, core_id);

        // If buddy works, we get a 4KB allocation
        if (fallback != static_cast<UInt64>(-1))
        {
            this->log("Buddy allocator succeeded, result=", fallback);
            stats.four_kb_allocated++;
            return std::make_pair(fallback, 12);
        }

        // If buddy fails, try demoting a partially used 2MB region
        this->log("Buddy allocator failed, attempting to demote a page");
        if (demote_page())
        {
            auto retried = buddy_allocator->allocate(size, address, core_id);
            stats.four_kb_allocated++;
            return std::make_pair(retried, 12);
        }
        else
        {
            assert(false);
            // No pages left to demote => out of memory
            return make_pair((UInt64)-1, 12);
        }
    }

    /*
    * allocate_ranges(...) => Not used in this model. Could be extended if needed.
    */
    std::vector<Range> allocate_ranges(IntPtr /*start_va*/, IntPtr /*end_va*/, int /*app_id*/)
    {
        std::vector<Range> ranges;
        return ranges;
    }


    /*
    * fragment_memory(...) => forcibly fragment memory in the buddy allocator, used for testing.
    * Uses the currently configured fragmentation mode (RATIO_BASED or COUNT_BASED).
    */
    void fragment_memory(double target_fragmentation)
    {
        if (m_frag_mode == FragmentationMode::COUNT_BASED) {
            // In COUNT_BASED mode, ignore the ratio and use the configured count
            buddy_allocator->fragmentMemoryToTargetCount(m_target_free_2mb_count);
        } else {
            // In RATIO_BASED mode, use the provided ratio
            buddy_allocator->fragmentMemory(target_fragmentation);
        }
    }
    
    /**
     * fragment_memory_to_count(UInt64 target_free_2mb_pages):
     *   Directly fragment memory to leave exactly the specified number of 2MB pages free.
     *   This method temporarily switches to COUNT_BASED mode, performs fragmentation,
     *   and can be used regardless of the current mode setting.
     *
     * @param target_free_2mb_pages The exact number of free 2MB pages to maintain
     */
    void fragment_memory_to_count(UInt64 target_free_2mb_pages)
    {
        buddy_allocator->fragmentMemoryToTargetCount(target_free_2mb_pages);
    }
    
    /**
     * setFragmentationMode(FragmentationMode mode):
     *   Set the fragmentation mode to either RATIO_BASED or COUNT_BASED.
     *
     * @param mode The fragmentation mode to use
     */
    void setFragmentationMode(FragmentationMode mode)
    {
        m_frag_mode = mode;
    }
    
    /**
     * getFragmentationMode():
     *   Returns the current fragmentation mode.
     */
    FragmentationMode getFragmentationMode() const
    {
        return m_frag_mode;
    }
    
    /**
     * setTargetFragmentationRatio(double ratio):
     *   Set the target fragmentation ratio for RATIO_BASED mode.
     *   This is the ratio of free 2MB pages to total 2MB pages (0.0 to 1.0).
     *
     * @param ratio The target ratio (0.0 = fully fragmented, 1.0 = no fragmentation)
     */
    void setTargetFragmentationRatio(double ratio)
    {
        m_target_frag_ratio = ratio;
    }
    
    /**
     * setTargetFree2MBCount(UInt64 count):
     *   Set the exact number of free 2MB pages to maintain for COUNT_BASED mode.
     *
     * @param count The exact number of free 2MB pages to keep available
     */
    void setTargetFree2MBCount(UInt64 count)
    {
        m_target_free_2mb_count = count;
    }
    
    /**
     * getTargetFree2MBCount():
     *   Returns the configured target number of free 2MB pages.
     */
    UInt64 getTargetFree2MBCount() const
    {
        return m_target_free_2mb_count;
    }
    
    /**
     * getCurrentFree2MBCount():
     *   Returns the current number of free 2MB pages available in the allocator.
     */
    UInt64 getCurrentFree2MBCount() const
    {
        return buddy_allocator->getFreeLargePageCount();
    }
    
    /**
     * configureCountBasedFragmentation(UInt64 target_free_2mb_pages):
     *   Convenience method to switch to COUNT_BASED mode and set the target count.
     *   After calling this, subsequent calls to fragment_memory() will use count-based
     *   fragmentation.
     *
     * @param target_free_2mb_pages The exact number of free 2MB pages to maintain
     */
    void configureCountBasedFragmentation(UInt64 target_free_2mb_pages)
    {
        m_frag_mode = FragmentationMode::COUNT_BASED;
        m_target_free_2mb_count = target_free_2mb_pages;
    }
    
    /**
     * configureRatioBasedFragmentation(double target_ratio):
     *   Convenience method to switch to RATIO_BASED mode and set the target ratio.
     *   After calling this, subsequent calls to fragment_memory() will use ratio-based
     *   fragmentation.
     *
     * @param target_ratio The target ratio of free 2MB pages (0.0 to 1.0)
     */
    void configureRatioBasedFragmentation(double target_ratio)
    {
        m_frag_mode = FragmentationMode::RATIO_BASED;
        m_target_frag_ratio = target_ratio;
    }



    UInt64 givePageFast(UInt64 bytes, UInt64 address = 0, UInt64 core_id = -1) override
    {
        this->log("givePageFast: bytes=", bytes, "addr=", address, "core=", core_id);
        return buddy_allocator->allocate(bytes, address, core_id);
    }

    /*
    * deallocate(...) => Not implemented. Could free 4KB or 2MB pages from the relevant structures.
    */
    void deallocate(UInt64 /*region*/, UInt64 /*core_id*/ = -1) override
    {
        // TODO: Implement
    }

    IntPtr isLargePageReserved(IntPtr address) { // /* SpecTLB spec engine */ 
        if (two_mb_map.find(address >> 21) != two_mb_map.end())
            return get<0>(two_mb_map[address >> 21]);
        return -1;
    }

    // Accessors for kcompactd and other OS daemons
    BuddyType* getBuddyAllocator() { return buddy_allocator; }
    std::map<UInt64, std::tuple<UInt64, std::bitset<512>, bool>>& getTwoMbMap() { return two_mb_map; }
protected:
    BuddyType* buddy_allocator;
    std::map<UInt64, std::tuple<UInt64, std::bitset<512>, bool>> two_mb_map;
    double threshold_for_promotion;

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
    bool demote_page()
    {
        // We'll collect <2MB_index, utilization> pairs and sort them by ascending utilization
        std::vector<std::pair<UInt64, double>> utilization;

        this->log("demote_page: sorting regions by utilization");

        for (auto it = two_mb_map.begin(); it != two_mb_map.end(); it++)
        {
            // Skip any regions already "promoted" (fully used or mapped at 2MB)
            if (std::get<2>(it->second))
                continue;

            double util = static_cast<double>(std::get<1>(it->second).count()) / 512;
            utilization.push_back(std::make_pair(it->first, util));
        }

        // Sort by ascending utilization
        std::sort(utilization.begin(), utilization.end(), [](std::pair<UInt64, double> &left, std::pair<UInt64, double> &right)
                { return left.second < right.second; });

        this->log("Sorted utilization vector, count=", utilization.size());

        // If there's no 2MB region to demote, return false
        if (utilization.size() == 0)
        {
            this->log("No regions to demote");
            return false;
        }

        // Remove the region_2MB from the two_mb_map
        UInt64 region_2MB = utilization[0].first;
        UInt64 region_begin = get<0>(two_mb_map[region_2MB]);
        int region_size = 512; // 512 pages of 4KB each => 2MB total
        int chunk = region_size;

        this->log("Removing region_2MB with utilization=", utilization[0].second);

        for (int j = 0; j < chunk; j++)
        {
            // if the bit is set, then the page is allocated so don't push it
            if (get<1>(two_mb_map[region_2MB])[j])
                continue;

            buddy_allocator->free(region_begin + j, region_begin + (j + 1));
        }

        int pages_freed = 512 - get<1>(two_mb_map[region_2MB]).count();
        this->log("Demoted 2MB region, utilization=", utilization[0].second, "freed_pages=", pages_freed);

        stats.two_mb_demoted++;

        // Remove this region from the map entirely
        two_mb_map.erase(utilization[0].first);
        this->log("Erased region_2MB from two_mb_map");

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
    std::pair<UInt64, UInt64> checkFor2MBAllocation(UInt64 address, UInt64 core_id)
    {

        UInt64 region_2MB = address >> 21; // Calculate the 2MB region index
        // offset_in_2MB is the 4KB-page index within that chunk
        // We'll recalc it if we do find or create a region
        [[maybe_unused]] int offset_in_2MB = (address >> 12) & 0x1FF;

        this->log("checkFor2MB: region=", region_2MB, "offset=", offset_in_2MB);

        // If we have not yet reserved that 2MB region, try to do so now
        if(two_mb_map.find(region_2MB) == two_mb_map.end()){
            this->log("region_2MB not found in two_mb_map, reserving");

            auto two_mb_reserved_region = buddy_allocator->reserve_2mb_page(address, core_id);
            this->log("reserve_2mb_page result=", get<0>(two_mb_reserved_region));

            // If we couldn't reserve 2MB, return -1
            if(get<0>(two_mb_reserved_region) == static_cast<UInt64>(-1)) {
                this->log("Failed to reserve 2MB region");
                return std::make_pair(-1, false);
            }
            else{
                // We successfully reserved a 2MB chunk, create a new entry in two_mb_map
                two_mb_map[region_2MB] = make_tuple(get<0>(two_mb_reserved_region), bitset<512>(),
                                                    false /* not promoted */);    
                stats.two_mb_reserved++;
                this->log("Reserved 2MB region, total_reserved=", stats.two_mb_reserved);
            } 
        }

        // Retrieve the region info from two_mb_map
		auto& region = two_mb_map[region_2MB];

            this->log("Debug: Retrieved region from two_mb_map");

            // If region has already been "promoted," we logically shouldn't have a page fault here
            if (std::get<2>(region))
            {
                this->log("Debug: Page is already promoted");
                // If the page was promoted, we wouldn't typically be calling this. So it's an assert.
                assert(false);
            }
		else { // If the page is not promoted, check if it should be promoted

            // Mark the correct offset within the 2MB region as in use
			auto& bitset = std::get<1>(region);
			int offset_in_2MB = (address >> 12) & 0x1FF;

                this->log("Debug: Page is not promoted, recalculated offset_in_2MB=", offset_in_2MB);

                bitset.set(offset_in_2MB); // Mark the page as used

                // Bitset logging removed - too verbose

            // Compute the utilization fraction for this region
            float utilization = static_cast<float>(bitset.count()) / 512;

            this->log("Debug: Calculated utilization=", utilization, " threshold_for_promotion=", threshold_for_promotion);
            // If utilization exceeds the threshold, we "promote" the entire region as a huge page
            bool ready_to_promote = (utilization > threshold_for_promotion);
            if (ready_to_promote && !std::get<2>(region))
            {
                std::get<2>(region) = true;  // Mark as promoted
                stats.two_mb_promoted++;

                this->log("Promoted page, total_promoted=", stats.two_mb_promoted);
                // Return the base PPN of the 2MB region for promoted pages
                // The MMU will use the 21-bit offset from the VA to address within the 2MB page
                // Note: std::get<0>(region) is the base PFN (4KB granularity) of the 2MB region
                return std::make_pair(std::get<0>(region), true);
            }
            else
            {
                // If not promoted, just return the PPN within the 2MB region
                // Note: std::get<0>(region) is a PFN from buddy allocator, offset_in_2MB is page index (0-511)
                this->log("Not promoted, returning PPN=", std::get<0>(region) + offset_in_2MB);
                return std::make_pair(std::get<0>(region) + offset_in_2MB, false);
            }
        }

        // Should not reach here
        return std::make_pair((UInt64)-1, false);
    }

};
