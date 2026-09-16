#pragma once
#include "fixed_types.h"
#include <vector>
#include <tuple>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <string>
#include <random>
#include <set>
#include <unordered_set>

#include "debug_config.h"

// Enable duplicate allocation detection for debugging (set to 0 for production)
#define BUDDY_DUPLICATE_DETECTION 0

/* Apr 19 2026: macro-gated -O0 for MimicOS-side calibration.
 * When this header is included from the MimicOS userspace binary, we
 * compile the allocator at -O0 so the simulated x86 instructions emitted
 * for each buddy operation match what we calibrated against real Linux
 * (phys_alloc mean ~2 300 cyc).  When included from Sniper's sniper-
 * space path, we stay at whatever optimisation level Sniper was built
 * at (typically -O3) so native in-simulator fault handling stays fast.
 * MimicOS defines MIMICOS_CALIBRATION_O0 via its Makefile. */
#ifdef MIMICOS_CALIBRATION_O0
#pragma GCC push_options
#pragma GCC optimize ("O0")
#endif

using namespace std;

template <typename Policy>
class Buddy : private Policy
{
public:
    Buddy(int memory_size, int max_order, int kernel_size, const String& frag_type)
        : m_memory_size(memory_size),
          m_max_order(max_order),
          m_kernel_size(kernel_size),
          m_frag_type(frag_type)
    {

        Policy::on_init(m_memory_size, m_max_order, m_kernel_size);
        
#if DEBUG_BUDDY >= DEBUG_BASIC
        // Initialize free lists
        this->log("Debug: Initializing Buddy allocator with memory_size = " + std::to_string(memory_size) +
                  ", max_order = "   + std::to_string(max_order) +
                  ", kernel_size = " + std::to_string(kernel_size) +
                  ", frag_type = "   + frag_type.c_str());
#endif  
        free_list.resize(m_max_order + 1);
        for (int i = 0; i < m_max_order + 1; i++)
            free_list.emplace_back();

        if (frag_type == "contiguity")
            frag_fun = &Buddy::getAverageSizeRatio;
        else
            frag_fun = &Buddy::getLargePageRatio;

        std::cout << "[Buddy] Memory Size: " << m_memory_size << std::endl;
        UInt64 pages_in_block = 1ULL << m_max_order;
        UInt64 current_order = m_max_order;

        UInt64 total_mem_in_pages = static_cast<UInt64>(m_memory_size) * 1024 / 4 - static_cast<UInt64>(m_kernel_size) * 1024 / 4;
        m_total_pages = total_mem_in_pages;
        m_free_pages = m_total_pages;

        UInt64 available_mem_in_pages = total_mem_in_pages;
        UInt64 current_free = static_cast<UInt64>(m_kernel_size) * 1024 / 4;

        std::cout << "[Buddy] 4KB pages in memory: " << total_mem_in_pages << std::endl;
        std::cout << "[Buddy] 2MB pages in memory: " << total_mem_in_pages / 512 << std::endl;
        std::cout << "[Buddy] 1GB pages in memory: " << total_mem_in_pages / 512 / 512 << std::endl;
        while (current_free < (static_cast<UInt64>(m_memory_size) * 1024 / 4))
        {
            while (available_mem_in_pages >= pages_in_block)
            {
#if DEBUG_BUDDY >= DEBUG_DETAILED
                this->log("[Buddy] Adding block of size " + std::to_string(pages_in_block) +
                " at address " + std::to_string(current_free)); 
#endif
                free_list[current_order].push_back(std::make_tuple(current_free, current_free + pages_in_block - 1, false, -1));
                current_free += pages_in_block;
                available_mem_in_pages -= pages_in_block;
            }
#if DEBUG_BUDDY >= DEBUG_BASIC
		this->log("[Buddy] Order " + std::to_string(current_order) +
        " has " + std::to_string(free_list[current_order].size()) + " blocks");
#endif
            current_order--;
            pages_in_block = 1ULL << current_order;
        }

    }

    int getMaxOrder() const { return m_max_order; }

    bool checkIfFree(UInt64 physical_page, bool allocate)
    {
#if DEBUG_BUDDY >= DEBUG_BASIC
        this->log("[Buddy::checkIfFree] Checking page " + std::to_string(physical_page) + ", allocate: " + (allocate ? "true" : "false"));
#endif
        int found_order = -1;
        int found_block_index = -1;
        bool found = false;

        int iterations = 0;
        // Search for the physical page in the free lists, starting from order 0.
        for (int i = 0; i <= m_max_order; i++)
        {
            for (size_t j = 0; j < free_list[i].size(); ++j)
            {
                if (get<0>(free_list[i][j]) <= physical_page && get<1>(free_list[i][j]) >= physical_page)
                {
#if DEBUG_BUDDY >= DEBUG_BASIC
                    this->log("[Buddy::checkIfFree] Found page " + std::to_string(physical_page) + " in block at order " + std::to_string(i) + ", index " + std::to_string(j));
                    this->log("[Buddy::checkIfFree] Block details: start=" + std::to_string(get<0>(free_list[i][j])) + ", end=" + std::to_string(get<1>(free_list[i][j])));
#endif
                    found = true;
                    found_order = i;
                    found_block_index = j;

                    goto found_block; // Exit nested loops once the block is found
                }
                iterations++;
            }
        }

    found_block:
    std::cout << "[Buddy::checkIfFree] Iterations: " << iterations << std::endl;
        if (!found)
        {
#if DEBUG_BUDDY >= DEBUG_BASIC
            this->log("[Buddy::checkIfFree] Page " + std::to_string(physical_page) + " is not in any free block.");
#endif
            return false; // The page is not in any free block.
        }

        if (allocate)
        {
#if DEBUG_BUDDY >= DEBUG_BASIC
            this->log("[Buddy::checkIfFree] Allocation requested. Starting split process.");
#endif
            // The page is free and we need to allocate it.
            // This involves removing the block and splitting it until we isolate the single page.
            
            // 1. Get the block that contains the page and remove it from its free list.
            std::tuple<UInt64, UInt64, bool, UInt64> block_to_split = free_list[found_order][found_block_index];
            free_list[found_order].erase(free_list[found_order].begin() + found_block_index);

#if DEBUG_BUDDY >= DEBUG_BASIC
            this->log("[Buddy::checkIfFree] Removed block from order " + std::to_string(found_order) + " to start splitting.");
#endif

            // 2. Recursively split the block until we reach order 0 (a single page).
            for (int current_order = found_order; current_order > 0; --current_order)
            {
#if DEBUG_BUDDY >= DEBUG_BASIC
                this->log("[Buddy::checkIfFree] Splitting block of order " + std::to_string(current_order) + ". Start: " + std::to_string(get<0>(block_to_split)) + ", End: " + std::to_string(get<1>(block_to_split)));
#endif
                UInt64 start_addr = get<0>(block_to_split);
                UInt64 end_addr = get<1>(block_to_split);
                UInt64 mid_point = start_addr + (end_addr - start_addr) / 2;
                
                std::tuple<UInt64, UInt64, bool, UInt64> buddy1, buddy2;
                
                buddy1 = std::make_tuple(start_addr, mid_point, false, -1);
                buddy2 = std::make_tuple(mid_point + 1, end_addr, false, -1);
                
                // Determine which buddy contains the physical page.
                // The other buddy is returned to the free list.
                if (physical_page <= mid_point)
                {
#if DEBUG_BUDDY >= DEBUG_BASIC
                    this->log("[Buddy::checkIfFree] Page in first buddy. Returning second buddy (start=" + std::to_string(get<0>(buddy2)) + ", end=" + std::to_string(get<1>(buddy2)) + ") to order " + std::to_string(current_order - 1));
#endif
                    // The page is in the first buddy. Add the second buddy to the free list.
                    free_list[current_order - 1].push_back(buddy2);
                    block_to_split = buddy1; // Continue splitting the first buddy.
                }
                else
                {
#if DEBUG_BUDDY >= DEBUG_BASIC
                    this->log("[Buddy::checkIfFree] Page in second buddy. Returning first buddy (start=" + std::to_string(get<0>(buddy1)) + ", end=" + std::to_string(get<1>(buddy1)) + ") to order " + std::to_string(current_order - 1));
#endif
                    // The page is in the second buddy. Add the first buddy to the free list.
                    free_list[current_order - 1].push_back(buddy1);
                    block_to_split = buddy2; // Continue splitting the second buddy.
                }
            }
            
            // 3. Decrement the free page count. The 'block_to_split' is now the single page being allocated.
            m_free_pages--;
#if DEBUG_BUDDY >= DEBUG_BASIC
            this->log("[Buddy::checkIfFree] Allocation complete. Final allocated page: " + std::to_string(get<0>(block_to_split)) + ". Free pages left: " + std::to_string(m_free_pages));
#endif
        }

        // If we found the block, the page is considered free, regardless of allocation.
        return true;
    }

#ifdef MIMICOS_CALIBRATION_O0
    __attribute__((noinline, optimize("O0")))
#endif
    UInt64 allocate(UInt64 bytes, UInt64 address = 0, UInt64 core_id = -1)
    {
        int ind = ceil(log2(bytes / 4096));
        int i;
        for (i = ind; i <= m_max_order; i++)
            if (!free_list[i].empty()) break;

        if (i == m_max_order + 1)
        {
            Policy::on_out_of_memory(bytes, address, core_id);
            return static_cast<UInt64>(-1);
        }

        auto block = free_list[i].back();
#if DEBUG_BUDDY >= DEBUG_BASIC
		this->log("[Buddy] Found free page in order " + std::to_string(i));
#endif
        free_list[i].pop_back();
        i--;

        while (i >= ind)
        {
            auto pair1 = std::make_tuple(get<0>(block), get<0>(block) + (get<1>(block) - get<0>(block)) / 2, false, -1);
            auto pair2 = std::make_tuple(get<0>(block) + (get<1>(block) - get<0>(block)) / 2 + 1, get<1>(block), false, -1);
			assert((get<1>(pair2) - get<0>(pair2) + 1) == (1ULL << i));
            free_list[i].push_back(pair1);
            free_list[i].push_back(pair2);
            block = free_list[i].back();
            free_list[i].pop_back();
            i--;
        }

        UInt64 allocated_page = get<0>(block);

#if BUDDY_DUPLICATE_DETECTION
        // Check for duplicate allocation
        UInt64 num_pages = 1ULL << ind;
        for (UInt64 p = 0; p < num_pages; p++) {
            UInt64 page_num = allocated_page + p;
            if (m_allocated_pages.find(page_num) != m_allocated_pages.end()) {
                std::cerr << "[BUDDY ERROR] DUPLICATE ALLOCATION DETECTED!" << std::endl;
                std::cerr << "  Page " << page_num << " (4KB page #) was already allocated!" << std::endl;
                std::cerr << "  Requested bytes: " << bytes << ", order: " << ind << std::endl;
                std::cerr << "  Block start: " << allocated_page << ", Block end: " << get<1>(block) << std::endl;
                m_duplicate_count++;
                assert(false && "Duplicate page allocation detected!");
            }
            m_allocated_pages.insert(page_num);
        }
#endif

#if DEBUG_BUDDY >= DEBUG_BASIC
		  this->log("[Buddy] Allocated " + std::to_string(bytes) +
            " bytes at address " + std::to_string(allocated_page));
#endif
        m_free_pages -= (1ULL << ind);
        return allocated_page;
    }

    /* ------------------------------------------------------------------
     *  allocate_batch_order0
     * ------------------------------------------------------------------
     *  Pulls up to `batch_size` × 4 KB pages in a single, tight loop and
     *  appends their PFNs to `out_pfns`.  Structurally mimics Linux's
     *  rmqueue_bulk in mm/page_alloc.c: one zone_lock acquire, N cheap
     *  pops from the order-0 list, an occasional expand()-style split
     *  chain when that list runs dry.
     *
     *  The reason this exists: the linux_buddy_anon allocator's per-CPU
     *  pageset refill used to call allocate(4096) in a 32-iteration loop,
     *  paying the per-call overhead (ceil/log2, full order-list scan,
     *  stats atomics, function-call/stack-frame setup) on every iteration.
     *  Sniper-simulated that at ~2 500 cyc/page ⇒ ~80 000 cyc per refill,
     *  vs real Linux's ~2 500 cyc for the whole 32-page batch.  Using this
     *  tight loop brings per-refill cost ~30× closer to measured Linux.
     *  Returns the number of pages actually appended (< batch_size only
     *  if the buddy ran out of memory).
     * ------------------------------------------------------------------ */
#ifdef MIMICOS_CALIBRATION_O0
    __attribute__((noinline, optimize("O0")))
#endif
    UInt64 allocate_batch_order0(UInt64 batch_size,
                                 std::vector<UInt64>& out_pfns,
                                 UInt64 core_id = (UInt64)-1)
    {
        UInt64 allocated = 0;
        out_pfns.reserve(out_pfns.size() + batch_size);

        while (allocated < batch_size) {
            if (!free_list[0].empty()) {
                /* Hot path: pop a pre-split order-0 block.  Linux equivalent:
                   list_first_entry + list_del in __rmqueue_smallest(order=0). */
                auto block = free_list[0].back();
                free_list[0].pop_back();
                out_pfns.push_back(std::get<0>(block));
                m_free_pages -= 1;
                allocated++;
                continue;
            }

            /* Cold path: order-0 empty, split down from the lowest non-empty
               higher order.  Mirrors expand() in mm/page_alloc.c. */
            int i = 1;
            while (i <= m_max_order && free_list[i].empty()) i++;
            if (i > m_max_order) {
                Policy::on_out_of_memory(4096, 0, core_id);
                return allocated;
            }
            auto block = free_list[i].back();
            free_list[i].pop_back();

            while (i > 0) {
                auto lo  = std::get<0>(block);
                auto hi  = std::get<1>(block);
                auto mid = lo + (hi - lo) / 2;
                auto pair1 = std::make_tuple(lo,       mid, false, (UInt64)-1);
                auto pair2 = std::make_tuple(mid + 1,  hi,  false, (UInt64)-1);
                i--;
                free_list[i].push_back(pair1);
                free_list[i].push_back(pair2);
                block = free_list[i].back();
                free_list[i].pop_back();
            }

            out_pfns.push_back(std::get<0>(block));
            m_free_pages -= 1;
            allocated++;
        }
        return allocated;
    }

    std::tuple<UInt64, UInt64, bool, UInt64> reserve_2mb_page(UInt64 address, UInt64 core_id)
    {
	// check if there is a 2MB region available
#if DEBUG_BUDDY >= DEBUG_BASIC
        this->log("DEBUG_BUDDY: Checking for 2MB region in free_list[9]");
#endif
        if (!free_list[9].empty())
        {
#if DEBUG_BUDDY >= DEBUG_BASIC
            this->log("DEBUG_BUDDY: 2MB region available in free_list[9]");
#endif
            auto block = free_list[9].back();
            free_list[9].pop_back();
            
#if BUDDY_DUPLICATE_DETECTION
            UInt64 region_start = get<0>(block);
            if (m_reserved_2mb_regions.find(region_start) != m_reserved_2mb_regions.end()) {
                std::cerr << "[BUDDY ERROR] DUPLICATE 2MB REGION DETECTED!" << std::endl;
                std::cerr << "  2MB Region starting at page " << region_start << " was already reserved!" << std::endl;
                m_2mb_duplicate_count++;
                assert(false && "Duplicate 2MB region reservation detected!");
            }
            m_reserved_2mb_regions.insert(region_start);
            // Also mark all 512 pages within this 2MB region as allocated
            for (UInt64 p = 0; p < 512; p++) {
                if (m_allocated_pages.find(region_start + p) != m_allocated_pages.end()) {
                    std::cerr << "[BUDDY ERROR] 2MB reservation overlaps with already allocated page!" << std::endl;
                    std::cerr << "  Page " << (region_start + p) << " was already allocated!" << std::endl;
                    m_duplicate_count++;
                    assert(false && "2MB reservation overlaps with allocated page!");
                }
                m_allocated_pages.insert(region_start + p);
            }
#endif
            
#if DEBUG_BUDDY >= DEBUG_BASIC
            this->log("DEBUG_BUDDY: Retrieved and removed 2MB region from free_list[9]");
#endif
            return block;
        }

#if DEBUG_BUDDY >= DEBUG_BASIC
        this->log("DEBUG_BUDDY: No 2MB region available in free_list[9], checking higher orders");
#endif
        for (int i = 10; i <= m_max_order; i++)
        {
            if (!free_list[i].empty())
            {
#if DEBUG_BUDDY >= DEBUG_BASIC
			this->log("DEBUG_BUDDY: Region available in free_list[" + std::to_string(i) + "]");
#endif
                auto temp = free_list[i].front();
                free_list[i].erase(free_list[i].begin());

                UInt64 start = get<0>(temp);
                UInt64 end = get<1>(temp);
                UInt64 size = end - start + 1;
                UInt64 chunk = size / 512;
                UInt64 pages_in_block = 1ULL << 9;

#if DEBUG_BUDDY >= DEBUG_BASIC
                this->log("DEBUG_BUDDY: Splitting region from free_list[" + std::to_string(i) +  "] into 2MB chunks");
#endif

                for (UInt64 j = 0; j < chunk; j++)
                {
                    free_list[9].push_back(std::make_tuple(start + j * pages_in_block, start + (j + 1) * pages_in_block - 1, false, -1));
					assert(get<0>(free_list[9].back()) == (start + j * pages_in_block));
#if DEBUG_BUDDY >= DEBUG_BASIC
					this->log("DEBUG_BUDDY: Added 2MB chunk to free_list[9], start = " + std::to_string(start + j * pages_in_block));
#endif
                }

                auto block = free_list[9].back();
                free_list[9].pop_back();
                
#if BUDDY_DUPLICATE_DETECTION
                UInt64 region_start = get<0>(block);
                if (m_reserved_2mb_regions.find(region_start) != m_reserved_2mb_regions.end()) {
                    std::cerr << "[BUDDY ERROR] DUPLICATE 2MB REGION DETECTED (after split)!" << std::endl;
                    std::cerr << "  2MB Region starting at page " << region_start << " was already reserved!" << std::endl;
                    m_2mb_duplicate_count++;
                    assert(false && "Duplicate 2MB region reservation detected!");
                }
                m_reserved_2mb_regions.insert(region_start);
                // Also mark all 512 pages within this 2MB region as allocated
                for (UInt64 p = 0; p < 512; p++) {
                    if (m_allocated_pages.find(region_start + p) != m_allocated_pages.end()) {
                        std::cerr << "[BUDDY ERROR] 2MB reservation overlaps with already allocated page!" << std::endl;
                        std::cerr << "  Page " << (region_start + p) << " was already allocated!" << std::endl;
                        m_duplicate_count++;
                        assert(false && "2MB reservation overlaps with allocated page!");
                    }
                    m_allocated_pages.insert(region_start + p);
                }
#endif
                
#if DEBUG_BUDDY >= DEBUG_BASIC
				this->log("DEBUG_BUDDY: Retrieved and removed 2MB region from free_list[9] after splitting");
				this->log("DEBUG_BUDDY: Returning 2MB region from 4KB start page: " + std::to_string(get<0>(temp)) + " and end page: " + std::to_string(get<1>(temp)));
#endif
                return block;
            }
        }

#if DEBUG_BUDDY >= DEBUG_BASIC
		this->log("DEBUG_BUDDY: No region available, returning nullptr");
#endif
        return std::make_tuple((UInt64)-1, 0, false, 0);
    }

    void free(UInt64 start, UInt64 end)
    {
        int order = ceil(log2((end - start + 1)));
        int i = order;
#if DEBUG_BUDDY >= DEBUG_BASIC
		this->log("Debug: Calculated order = " + std::to_string(order));
#endif

        std::tuple<UInt64, UInt64, bool, UInt64> temp = std::make_tuple(start, end, false, -1);

#if DEBUG_BUDDY >= DEBUG_BASIC
            this->log("Debug: Created tuple with start = " +
                    std::to_string(start) +
                    " and end = " +
                    std::to_string(end));
#endif

        free_list[i].push_back(temp);

#if BUDDY_DUPLICATE_DETECTION
        // Remove freed pages from tracking set
        UInt64 num_pages = end - start + 1;
        for (UInt64 p = 0; p < num_pages; p++) {
            m_allocated_pages.erase(start + p);
        }
#endif

#if DEBUG_BUDDY >= DEBUG_BASIC
            this->log("Debug: Pushed tuple to free_list[" +
                std::to_string(i) +
                "]");
#endif

        m_free_pages += (1ULL << i);

#if DEBUG_BUDDY >= DEBUG_BASIC
            this->log("Debug: Updated m_free_pages = " + std::to_string(m_free_pages));
#endif
    }

    void fragmentMemory(double target_fragmentation)
    {
        std::cout << "[BUDDY] Fragmenting memory to achieve target fragmentation: " << target_fragmentation << std::endl;
        std::mt19937 gen(12345);
        double current_fragmentation = (this->*frag_fun)();
        while (current_fragmentation> target_fragmentation)
        {
            for (int i = m_max_order; i >= 9; i--)
            {
                if (!free_list[i].empty())
                {
                    auto temp = free_list[i][0];
                    free_list[i].erase(free_list[i].begin());

                    UInt64 start = get<0>(temp);
                    UInt64 end = get<1>(temp);
                    UInt64 size = end - start + 1;
                    // generate a random order between 8 and m_max_order-3, add seed
                    std::uniform_int_distribution<UInt64> dist(8, i - 1);

                    UInt64 random_order = dist(gen);
                    UInt64 chunk = size / (1ULL << random_order);
                    UInt64 pages_in_block = 1ULL << random_order;

                    // Essentially, we are splitting the large page into smaller pages
                    // and adding them to the free list
                    // This way we are increasing the fragmentation without actually allocating memory which is very useful for testing and evaluation
                    for (UInt64 j = 0; j < chunk; j++)
                    {
                        free_list[random_order].push_back(std::make_tuple(start + j * pages_in_block, start + (j + 1) * pages_in_block - 1, false, -1));
                        assert(get<0>(free_list[random_order].back()) == (start + j * pages_in_block));
                    }

                    break;
                }
            }

            current_fragmentation = (this->*frag_fun)();
#if DEBUG_BUDDY >= DEBUG_DETAILED
            this->log("[Artificial Fragmentation Generator] Current fragmentation: " + std::to_string(current_fragmentation));
#endif
        }

        std::cout << "[BUDDY] Initialized memory with final fragmentation: " << current_fragmentation << std::endl;
        std::cout << "[BUDDY] Free pages: " << m_free_pages << std::endl;
        std::cout << "[BUDDY] Free pages in (MB): " << m_free_pages * 4 / 1024 << std::endl;
        Policy::on_fragmentation_done();
    }

    double getAverageSizeRatio()
    {
        std::vector<UInt64> blockSizes;

        // Collect sizes of all free blocks
        for (const auto &list : free_list)
            for (const auto &block : list)
                blockSizes.push_back(get<1>(block) - get<0>(block) + 1); // check if the block is contiguous

        // Sort block sizes in descending order
        std::sort(blockSizes.rbegin(), blockSizes.rend());

        // Calculate average of top 50 blocks
        UInt64 totalSize = 0;
        int count = 0;
        for (auto size : blockSizes)
        {
            totalSize += size;
            if (++count == 50) break;
        }

        double averageSize = (count > 0) ? (double)totalSize / count : 0.0;
        return averageSize / (1ULL << (m_max_order - 3));
    }

    double getLargePageRatio()
    {
        int numberOfLargePages = 0;
        // Collect number of 2MB pages based on the free list
        for (const auto &list : free_list)
            for (const auto &block : list)
                if ((get<1>(block) - get<0>(block) + 1) >= 512)
                    numberOfLargePages += (get<1>(block) - get<0>(block) + 1) / 512;

        // calculate the ratio of available large pages to the total number of 2MB pages
        m_frag_factor = (double)numberOfLargePages / (m_total_pages / 512);
        // std::cout << "[BUDDY] Large page ratio: " << m_frag_factor << std::endl;
        return m_frag_factor;
    }

    /**
     * getFreeLargePageCount():
     *   Returns the exact count of free 2MB pages available in the allocator.
     *   A 2MB page consists of 512 contiguous 4KB pages.
     */
    UInt64 getFreeLargePageCount()
    {
        UInt64 count = 0;
        for (const auto &list : free_list)
            for (const auto &block : list)
                if ((get<1>(block) - get<0>(block) + 1) >= 512)
                    count += (get<1>(block) - get<0>(block) + 1) / 512;
        return count;
    }

    /**
     * fragmentMemoryToTargetCount(UInt64 target_free_2mb_pages):
     *   Fragments memory until exactly the specified number of 2MB pages remain free.
     *   This provides more precise control over fragmentation compared to ratio-based
     *   fragmentation.
     *
     * @param target_free_2mb_pages The exact number of free 2MB pages to maintain
     */
    void fragmentMemoryToTargetCount(UInt64 target_free_2mb_pages)
    {
        std::cout << "[BUDDY] Fragmenting memory to achieve target free 2MB pages: " << target_free_2mb_pages << std::endl;
        std::mt19937 gen(12345);
        UInt64 current_free_2mb = getFreeLargePageCount();
        
        std::cout << "[BUDDY] Initial free 2MB pages: " << current_free_2mb << std::endl;
        
        while (current_free_2mb > target_free_2mb_pages)
        {
            bool split_occurred = false;
            for (int i = m_max_order; i >= 9; i--)
            {
                if (!free_list[i].empty())
                {
                    auto temp = free_list[i][0];
                    free_list[i].erase(free_list[i].begin());

                    UInt64 start = get<0>(temp);
                    UInt64 end = get<1>(temp);
                    UInt64 size = end - start + 1;
                    
                    // Split to order 8 (256 pages = 1MB) to guarantee breaking 2MB alignment
                    // This ensures we reduce the 2MB page count
                    std::uniform_int_distribution<UInt64> dist(8, std::min(i - 1, 8));
                    UInt64 random_order = dist(gen);
                    UInt64 chunk = size / (1ULL << random_order);
                    UInt64 pages_in_block = 1ULL << random_order;

                    for (UInt64 j = 0; j < chunk; j++)
                    {
                        free_list[random_order].push_back(std::make_tuple(start + j * pages_in_block, start + (j + 1) * pages_in_block - 1, false, -1));
                    }
                    
                    split_occurred = true;
                    break;
                }
            }
            
            if (!split_occurred) {
                std::cout << "[BUDDY] Warning: Could not split any more blocks" << std::endl;
                break;
            }

            current_free_2mb = getFreeLargePageCount();
#if DEBUG_BUDDY >= DEBUG_DETAILED
            this->log("[Artificial Fragmentation Generator] Current free 2MB pages: " + std::to_string(current_free_2mb));
#endif
        }

        std::cout << "[BUDDY] Initialized memory with final free 2MB pages: " << current_free_2mb << std::endl;
        std::cout << "[BUDDY] Free pages: " << m_free_pages << std::endl;
        std::cout << "[BUDDY] Free pages in (MB): " << m_free_pages * 4 / 1024 << std::endl;
        Policy::on_fragmentation_done();
    }

    UInt64 getFreePages() const { return m_free_pages; }
    UInt64 getTotalPages() const { return m_total_pages; }

#if BUDDY_DUPLICATE_DETECTION
    // Check if a 4KB page has been allocated before (for debugging)
    bool isPageAllocated(UInt64 page_num) const {
        return m_allocated_pages.find(page_num) != m_allocated_pages.end();
    }
    
    // Check if a 2MB region has been reserved before (for debugging)
    bool is2MBRegionReserved(UInt64 region_start) const {
        return m_reserved_2mb_regions.find(region_start) != m_reserved_2mb_regions.end();
    }
    
    // Get duplicate detection stats
    UInt64 getDuplicateCount() const { return m_duplicate_count; }
    UInt64 get2MBDuplicateCount() const { return m_2mb_duplicate_count; }
#endif

private:
    int m_memory_size;
    int m_max_order;
    int m_kernel_size;
    String m_frag_type;
    double m_frag_factor;

    UInt64 m_total_pages;
    UInt64 m_free_pages;

    std::vector<std::vector<std::tuple<UInt64, UInt64, bool, UInt64>>> free_list;
    double (Buddy::*frag_fun)();

#if BUDDY_DUPLICATE_DETECTION
    // Track all allocated 4KB pages to detect duplicates
    std::unordered_set<UInt64> m_allocated_pages;
    // Track all reserved 2MB regions to detect duplicates
    std::unordered_set<UInt64> m_reserved_2mb_regions;
    // Count of detected duplicates
    UInt64 m_duplicate_count = 0;
    UInt64 m_2mb_duplicate_count = 0;
#endif
};

#ifdef MIMICOS_CALIBRATION_O0
#pragma GCC pop_options
#endif

