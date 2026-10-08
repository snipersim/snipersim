#pragma once
/*------------------------------------------------------------------------------
 *  SpotAllocator: contiguity-aware placement for Speculative Offset-based
 *  Translation (Alverti et al., "Enhancing and Exploiting Contiguity for
 *  Fast Memory Virtualization", ISCA 2020).
 *
 *  The first fault in a VMA picks a free physical range that can hold the
 *  whole VMA (from a map of the free contiguous ranges) and fixes the VMA's
 *  offset: offset = VPN - first PPN of the range.  Every later fault in that
 *  VMA asks the buddy allocator for exactly VPN - offset, so that VA and PA
 *  stay a constant distance apart across the VMA and the SpOT engine can
 *  predict the translation from the offset alone.  When the exact frame is
 *  taken, or the address lies in no VMA (a live run has none), the fault
 *  gets an ordinary buddy page.
 *
 *  Policy-based, like the other allocators: the Policy supplies the
 *  statistics, the log and the OS's VMA bookkeeping.
 *----------------------------------------------------------------------------*/
#include "debug_config.h"
#include "memory_management/physical_memory_allocators/physical_memory_allocator.h"
#include "memory_management/misc/vma.h"
#include <vector>
#include <cassert>
#include <cstdlib>

#include "templates_traits_config.h"
#include "memory_management/physical_memory_allocators/buddy_policy_traits.h"
#include "memory_management/physical_memory_allocators/buddy.h"

template <typename Policy>
class SpotAllocator : public PhysicalMemoryAllocator, private Policy
{
    using BuddyPolicy = typename BuddyPolicyFor<Policy>::type;
    using BuddyType   = Buddy<BuddyPolicy>;

    struct Stats {
        UInt64 vmas_placed = 0;             // VMAs whose offset was fixed on their first fault
        UInt64 vmas_placed_in_largest = 0;  // ... in the largest free range, none holding the whole VMA
        UInt64 offset_allocations = 0;      // faults served at exactly VPN - offset
        UInt64 fallback_allocations = 0;    // faults in a placed VMA whose frame was taken: a buddy page
        UInt64 no_vma_allocations = 0;      // faults outside any VMA: a buddy page
    } stats;

public:
    Stats& getStats() { return stats; }

    SpotAllocator(String name, UInt64 memory_size, int max_order, UInt64 kernel_size, String frag_type)
        : PhysicalMemoryAllocator(name, memory_size, kernel_size)
        , m_max_order(max_order)
    {
        static_assert(is_complete<BuddyPolicyFor<Policy>>::value,
                      "BuddyPolicyFor<Policy> is incomplete - include buddy_policy.h before spot.h");
        Policy::on_init(name, memory_size, kernel_size, max_order, frag_type, this);
        buddy_allocator = new BuddyType(memory_size, max_order, kernel_size, frag_type);

        // The free contiguous ranges, in 4 KB frames, [ppn, bounds] inclusive:
        // everything above the kernel region to start with.
        PhysicalRange initial_range = {};
        initial_range.ppn = kernel_size * 1024 / 4;
        initial_range.bounds = memory_size * 1024 / 4 - 1;
        contiguity_map.push_back(initial_range);
        Policy::log(this, "[SpOT] contiguity map starts with frames " + std::to_string(initial_range.ppn) +
                    " - " + std::to_string(initial_range.bounds));
    }

    ~SpotAllocator() { delete buddy_allocator; }

    std::pair<UInt64, UInt64> allocate(UInt64 size, UInt64 address = 0, UInt64 core_id = -1,
                                       bool is_pagetable_allocation = false,
                                       bool is_instruction_allocation = false) override
    {
        if (is_pagetable_allocation)
            return std::make_pair(handle_page_table_allocations(size), 12);
        if (is_instruction_allocation)
            return this->allocateInstruction(size);

        // The VMA the address lies in, through the OS: its index, its
        // offset (-1 until this allocator fixes it) and its size.
        auto [vma_index, current_offset, vma_size] = Policy::find_VMA_specs(address, core_id, this);

        if (vma_index == -1)
        {
            stats.no_vma_allocations++;
            UInt64 ppn = buddy_allocator->allocate(4096, address, core_id);
            Policy::log(this, "[SpOT] no VMA for " + std::to_string(address) + ": buddy frame " + std::to_string(ppn));
            return std::make_pair(ppn, 12);
        }

        if (current_offset == static_cast<int64_t>(-1))
        {
            // First fault in the VMA: pick the free range to place it in.
            int64_t vma_pages = static_cast<int64_t>(vma_size / 4096);
            int chosen = -1;
            int64_t largest = -1, largest_index = -1;
            size_t i = static_cast<size_t>(contiguity_map_pointer) % std::max<size_t>(contiguity_map.size(), 1);
            for (size_t visited = 0; visited < contiguity_map.size(); visited++)
            {
                int64_t range_pages = contiguity_map[i].bounds - contiguity_map[i].ppn + 1;
                if (range_pages >= vma_pages)
                {
                    chosen = i;
                    break;
                }
                if (range_pages > largest)
                {
                    largest = range_pages;
                    largest_index = i;
                }
                i = (i + 1) % contiguity_map.size();
            }

            if (chosen != -1)
            {
                stats.vmas_placed++;
                contiguity_map_pointer = (chosen + 1) % contiguity_map.size();
            }
            else if (largest_index != -1)
            {
                // Nothing holds the whole VMA: the largest range, as the
                // paper's allocator does, so that at least its head is
                // contiguous.
                chosen = largest_index;
                stats.vmas_placed++;
                stats.vmas_placed_in_largest++;
                contiguity_map_pointer = largest_index;
            }
            else
            {
                std::cerr << "[SpOT] no free range left in the contiguity map for address 0x"
                          << std::hex << address << std::dec << std::endl;
                return std::make_pair(static_cast<UInt64>(-1), 12);
            }

            IntPtr first_ppn = contiguity_map[chosen].ppn;
            current_offset = static_cast<int64_t>(address >> 12) - static_cast<int64_t>(first_ppn);
            Policy::setPhysicalOffset(core_id, vma_index, current_offset, this);
            Policy::log(this, "[SpOT] VMA " + std::to_string(vma_index) + " (" + std::to_string(vma_pages) +
                        " pages) placed at frame " + std::to_string(first_ppn) + ", offset " +
                        std::to_string(current_offset));
        }

        // The frame that keeps VA - PA constant across the VMA.
        int64_t requested_ppn = static_cast<int64_t>(address >> 12) - current_offset;
        IntPtr final_ppn;

        // The buddy allocator hands the frame out (checkIfFree with allocate)
        // if it is free, so that it cannot be given to a later buddy
        // request as well.
        if (requested_ppn >= 0 && buddy_allocator->checkIfFree(static_cast<UInt64>(requested_ppn), true))
        {
            final_ppn = static_cast<IntPtr>(requested_ppn);
            stats.offset_allocations++;
            Policy::incrementSuccessfulOffsetBasedAllocations(core_id, address, this);
            Policy::log(this, "[SpOT] frame " + std::to_string(final_ppn) + " at the VMA's offset for " +
                        std::to_string(address));
        }
        else
        {
            final_ppn = buddy_allocator->allocate(size, address, core_id);
            stats.fallback_allocations++;
            Policy::log(this, "[SpOT] frame " + std::to_string(requested_ppn) + " taken: buddy frame " +
                        std::to_string(final_ppn) + " for " + std::to_string(address));
            if (final_ppn == static_cast<IntPtr>(-1))
                return std::make_pair(static_cast<UInt64>(-1), 12);
        }

        removeFromContiguityMap(final_ppn);
        return std::make_pair(final_ppn, 12);
    }

    void deallocate(UInt64, UInt64) override {}

    std::vector<Range> allocate_ranges(IntPtr, IntPtr, int) override
    {
        return std::vector<Range>();
    }

    /*
     * Poison a fraction of memory in the contiguity map: target_fragmentation
     * of the 4 MB blocks are cut out of the free ranges (the largest range
     * each time, at a random block), so that the ranges a VMA can be placed
     * in shrink.  The buddy allocator is left as it is: the poisoned frames
     * are only unavailable to offset-based placement.
     */
    void fragment_memory(double target_fragmentation) override
    {
        const int block_pages = 1024;  // 4 MB of 4 KB frames
        UInt64 blocks_total = buddy_allocator->getTotalPages() / block_pages;
        UInt64 blocks_to_poison = static_cast<UInt64>(target_fragmentation * blocks_total);
        Policy::log(this, "[SpOT] poisoning " + std::to_string(blocks_to_poison) + " of " +
                    std::to_string(blocks_total) + " 4 MB blocks");

        for (UInt64 n = 0; n < blocks_to_poison && !contiguity_map.empty(); n++)
        {
            int r = largestRange();
            int64_t blocks_in_range = (contiguity_map[r].bounds - contiguity_map[r].ppn + 1) / block_pages;
            if (blocks_in_range == 0)
                break;  // every range is below 4 MB: nothing left to poison
            if (blocks_in_range <= 2)
            {
                // Keep one block, drop the rest of the range.
                contiguity_map[r].bounds = contiguity_map[r].ppn + block_pages - 1;
                continue;
            }
            // Cut one block out of the middle (never an edge block).
            int64_t block = 1 + (rand() % (blocks_in_range - 2));
            IntPtr cut = contiguity_map[r].ppn + block * block_pages;
            PhysicalRange left = { contiguity_map[r].ppn, cut - 1 };
            PhysicalRange right = { cut + block_pages, contiguity_map[r].bounds };
            contiguity_map[r] = left;
            contiguity_map.insert(contiguity_map.begin() + r + 1, right);
        }
    }

    UInt64 givePageFast(UInt64 bytes, UInt64 address = 0, UInt64 core_id = -1) override
    {
        return buddy_allocator->allocate(bytes, address, core_id);
    }

protected:
    BuddyType *buddy_allocator;

private:
    friend Policy;

    int m_max_order;
    std::vector<PhysicalRange> contiguity_map;
    int contiguity_map_pointer = 0;

    int largestRange() const
    {
        int best = 0;
        int64_t best_pages = -1;
        for (size_t i = 0; i < contiguity_map.size(); i++)
        {
            int64_t pages = contiguity_map[i].bounds - contiguity_map[i].ppn + 1;
            if (pages > best_pages)
            {
                best_pages = pages;
                best = i;
            }
        }
        return best;
    }

    // Take one frame out of the free ranges: shrink the range it is at the
    // edge of, or split the range it is inside.  A frame outside every
    // range (the buddy's choice landed in poisoned or kernel space) leaves
    // the map alone.
    void removeFromContiguityMap(IntPtr ppn)
    {
        for (size_t i = 0; i < contiguity_map.size(); i++)
        {
            PhysicalRange &range = contiguity_map[i];
            if (ppn < range.ppn || ppn > range.bounds)
                continue;
            if (range.ppn == ppn && range.bounds == ppn)
                contiguity_map.erase(contiguity_map.begin() + i);
            else if (range.ppn == ppn)
                range.ppn = ppn + 1;
            else if (range.bounds == ppn)
                range.bounds = ppn - 1;
            else
            {
                PhysicalRange after = { ppn + 1, range.bounds };
                range.bounds = ppn - 1;
                contiguity_map.insert(contiguity_map.begin() + i + 1, after);
            }
            return;
        }
    }
};
