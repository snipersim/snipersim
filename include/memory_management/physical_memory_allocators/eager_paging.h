#pragma once
/*------------------------------------------------------------------------------
 *  EagerPagingAllocator: the allocator of Redundant Memory Mappings
 *  (Karakostas et al., ISCA 2015).  A VMA is allocated whole, on its first
 *  fault, as the fewest physically contiguous ranges the buddy allocator
 *  can give: the largest power-of-two block that fits what is left of the
 *  VMA, again and again.  The exception handler maps every page of each
 *  range and records the range in the range table, which the range MMU
 *  translates from.
 *
 *  Policy-based, like the other allocators: the Policy supplies the
 *  statistics and the log.
 *----------------------------------------------------------------------------*/
#include "debug_config.h"
#include "memory_management/physical_memory_allocators/physical_memory_allocator.h"
#include "memory_management/misc/vma.h"
#include <vector>
#include <cmath>
#include <algorithm>

#include "templates_traits_config.h"
#include "memory_management/physical_memory_allocators/buddy_policy_traits.h"
#include "memory_management/physical_memory_allocators/buddy.h"

template <typename Policy>
class EagerPagingAllocator : public PhysicalMemoryAllocator, private Policy
{
    using BuddyPolicy = typename BuddyPolicyFor<Policy>::type;
    using BuddyType   = Buddy<BuddyPolicy>;

    struct Stats {
        UInt64 total_allocated_vmas = 0;              // VMAs laid out eagerly
        UInt64 total_allocated_physical_ranges = 0;   // contiguous ranges they took
        UInt64 total_allocated_pages = 0;             // frames in those ranges
        UInt64 single_page_allocations = 0;           // faults served one page at a time (outside any VMA)
    } stats;

public:
    Stats& getStats() { return stats; }

    EagerPagingAllocator(String name, UInt64 memory_size, int max_order, UInt64 kernel_size, String frag_type)
        : PhysicalMemoryAllocator(name, memory_size, kernel_size)
    {
        static_assert(is_complete<BuddyPolicyFor<Policy>>::value,
                      "BuddyPolicyFor<Policy> is incomplete - include buddy_policy.h before eager_paging.h");
        Policy::on_init(name, memory_size, kernel_size, max_order, frag_type, this);
        buddy_allocator = new BuddyType(memory_size, max_order, kernel_size, frag_type);
    }

    ~EagerPagingAllocator() { delete buddy_allocator; }

    // One page: the handler's fallback for an address outside every VMA.
    std::pair<UInt64, UInt64> allocate(UInt64 size, UInt64 address = 0, UInt64 core_id = -1,
                                       bool is_pagetable_allocation = false,
                                       bool is_instruction_allocation = false) override
    {
        if (is_pagetable_allocation)
            return std::make_pair(handle_page_table_allocations(size), 12);
        if (is_instruction_allocation)
            return this->allocateInstruction(size);
        UInt64 ppn = buddy_allocator->allocate(size, address, core_id);
        stats.single_page_allocations++;
        Policy::log(this, "[EagerPaging] single page for " + std::to_string(address) + ": frame " + std::to_string(ppn));
        return std::make_pair(ppn, 12);
    }

    UInt64 givePageFast(UInt64 bytes, UInt64 address = 0, UInt64 core_id = -1) override
    {
        return buddy_allocator->allocate(bytes, address, core_id);
    }

    void deallocate(UInt64, UInt64) override {}

    // The whole of [start_va, end_va) as contiguous ranges: each Range is
    // (vpn = first page, bounds = number of pages, offset = first frame).
    std::vector<Range> allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id) override
    {
        std::vector<Range> ranges;
        IntPtr current_vpn = start_va / 4096;
        int64_t remaining_pages = (end_va - start_va) / 4096;
        int max_order = buddy_allocator->getMaxOrder();
        Policy::log(this, "[EagerPaging] laying out " + std::to_string(remaining_pages) + " pages from VPN " +
                    std::to_string(current_vpn));

        while (remaining_pages > 0)
        {
            // The largest power-of-two block that fits, down to what the
            // buddy allocator can still give when it is fragmented.
            int order = std::min(static_cast<int>(std::log2(remaining_pages)), max_order);
            UInt64 ppn = static_cast<UInt64>(-1);
            while (order >= 0 && ppn == static_cast<UInt64>(-1))
            {
                ppn = buddy_allocator->allocate((UInt64)(1 << order) * 4096, current_vpn * 4096, app_id);
                if (ppn == static_cast<UInt64>(-1))
                    order--;
            }
            if (ppn == static_cast<UInt64>(-1))
            {
                std::cerr << "[EagerPaging] out of memory at VPN 0x" << std::hex << current_vpn << std::dec << std::endl;
                break;
            }
            int64_t block_pages = 1 << order;
            Range range;
            range.vpn = current_vpn;
            range.bounds = block_pages;
            range.offset = ppn;
            ranges.push_back(range);
            Policy::log(this, "[EagerPaging] " + std::to_string(block_pages) + " pages at frame " +
                        std::to_string(ppn) + " for VPN " + std::to_string(current_vpn));
            current_vpn += block_pages;
            remaining_pages -= block_pages;
            stats.total_allocated_physical_ranges++;
            stats.total_allocated_pages += block_pages;
        }
        stats.total_allocated_vmas++;
        return ranges;
    }

    void fragment_memory(double target_fragmentation) override
    {
        buddy_allocator->fragmentMemory(target_fragmentation);
    }

    UInt64 getFreePages() const { return buddy_allocator->getFreePages(); }
    UInt64 getTotalPages() const { return buddy_allocator->getTotalPages(); }

protected:
    BuddyType *buddy_allocator;
};
