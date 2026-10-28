#pragma once
/*------------------------------------------------------------------------------
 *  BaselineAllocator ‑ common implementation
 *  – Policy‑based: only Policy::on_init(...) and Policy::log(...) differ
 *  – Stats are shared; registering them is delegated to the Policy (Sniper only)
 *----------------------------------------------------------------------------*/
#include "debug_config.h"
#include "memory_management/physical_memory_allocators/physical_memory_allocator.h"
#include <map>
#include <deque>
#include <unordered_map>
#include <vector>
#include <tuple>
#include <utility>
#include <bitset>
#include <cassert>

#include "templates_traits_config.h"

#include "memory_management/physical_memory_allocators/buddy_policy_traits.h"   // map Reserve/Baseline‑policy → Buddy‑policy
#include "memory_management/physical_memory_allocators/buddy.h"

#include "debug_config.h"

template <typename Policy>
class BaselineAllocator : public PhysicalMemoryAllocator, private Policy
{
    using BuddyPolicy = typename BuddyPolicyFor<Policy>::type;
    using BuddyType   = Buddy<BuddyPolicy>;

    /*------------------------  statistics shared by both builds  --------------*/
    struct Stats {
        UInt64 total_allocations = 0;
        /* add more per‑page counters later if needed */
    } stats;

public:
    /* expose Stats for Sniper‑side metric registration */
    Stats& getStats() { return stats; }

    /*------------------------------ ctor / dtor --------------------------------*/
    BaselineAllocator(String name,
                      int    memory_size,
                      int    max_order,
                      int    kernel_size,
                      String frag_type)
        : PhysicalMemoryAllocator(name, memory_size, kernel_size)
    {

        /* let the concrete policy do its setup & logging */
        Policy::on_init(name, memory_size, kernel_size, this);

        static_assert(is_complete<BuddyPolicyFor<Policy>>::value,
            "BuddyPolicyFor<Policy> is incomplete – did you include the proper "
            "buddy_policy.h BEFORE including baseline_allocator.h ?");

        buddy_allocator = new BuddyType(memory_size, max_order, kernel_size, frag_type);
    }

    ~BaselineAllocator() { delete buddy_allocator; }

    /*--------------------------- public interface ------------------------------*/
    std::pair<UInt64,UInt64> allocate(UInt64 bytes,
                                      UInt64 address               = 0,
                                      UInt64 core_id               = (UInt64)-1,
                                      bool   is_pagetable_alloc    = false,
                                      bool   is_instruction_alloc  = false) override
    {
#if DEBUG_BASELINE_ALLOCATOR >= DEBUG_BASIC
	this->log("BaselineAllocator::allocate(" + std::to_string(bytes) +
     ", " + std::to_string(address) +
     ", " + std::to_string(core_id) +
     ", " + std::to_string(is_pagetable_alloc) +
     ", " + std::to_string(is_instruction_alloc) + ")");
#endif
        stats.total_allocations++;

        if (is_pagetable_alloc) { 	// If the allocation is for a page table, handle it separately
            UInt64 phys = handle_page_table_allocations(bytes);
            return {phys, 12}; // Return the physical address and the page size
        }
        // Instruction allocations go to the reserved instruction area
        else if (is_instruction_alloc)
        {
            return this->allocateInstruction(bytes);
        }
        else if (bytes == 4096)
        {
            return {take_from_run(address, core_id), 12};
        }
        else
        {
            UInt64 phys = buddy_allocator->allocate(bytes, address, core_id);		// Simply allocate the memory block using the buddy allocator
            return {phys, 12};
        }

    }

    std::vector<Range> allocate_ranges(IntPtr /*start_va*/,
                                       IntPtr /*end_va*/,
                                       int    /*app_id*/) override
    {
        return {};          // not used in baseline allocator
    }

    UInt64 givePageFast(UInt64 bytes,
                        UInt64 address = 0,
                        UInt64 core_id = (UInt64)-1) override
    {
        // Just for the sake of testing the buddy allocator
        return buddy_allocator->allocate(bytes, address, core_id);
    }

    void deallocate(UInt64 /*region_begin*/,
                    UInt64 /*core_id*/ = (UInt64)-1) override
    {
        /* not implemented in baseline allocator */
    }

    /* optional helper – mirrors original fragment_memory() */
    void fragment_memory(double target_fragmentation)
    {
        buddy_allocator->fragmentMemory(target_fragmentation);
    }

private:
    BuddyType* buddy_allocator {nullptr};

    /* Per-core runs of frames, as Linux's per-CPU page lists (the pcp lists,
       refilled from the buddy allocator in batches of about 63 pages).

       The buddy allocator hands out adjacent frames one after another, so
       applications faulting in turn on two cores shared out the frames in
       whatever order their faults happened to arrive, and each could end up
       with several pages of one cache colour and none of another.  Two copies
       of a 192 KiB pointer chase, which fits gainestown's private 256 KiB L2,
       took 3,074 L2 misses per core in some runs and 53,000 or 113,000 in
       others -- an artifact of the allocator, varying from run to run, not of
       the caches.  (The Virtuoso paper, ASPLOS 2025, describes MimicOS's
       allocator as a buddy allocator and says nothing of frame order across
       applications.)  Each core now takes its 4 KiB pages from a run of 64
       contiguous frames of its own, refilled from the buddy allocator, so an
       application's pages cover the colours evenly whatever the other cores
       do; every run then gives 3,074.  A fragmented memory with no 64-page
       block left falls back to single pages. */
    static const unsigned RUN_ORDER = 6;          // 64 pages, 256 KiB
    std::unordered_map<UInt64, std::deque<UInt64>> m_runs;

    UInt64 take_from_run(UInt64 address, UInt64 core_id)
    {
        std::deque<UInt64> &run = m_runs[core_id];
        if (run.empty())
        {
            const UInt64 base = buddy_allocator->allocate(4096ULL << RUN_ORDER, address, core_id);
            if (base == static_cast<UInt64>(-1))
                return buddy_allocator->allocate(4096, address, core_id);
            for (UInt64 i = 0; i < (1ULL << RUN_ORDER); i++)
                run.push_back(base + i);
        }
        const UInt64 frame = run.front();
        run.pop_front();
        return frame;
    }

    /* original helper for page‑table pages */
    UInt64 handle_page_table_allocations(UInt64 bytes)
    {
        /* 4 KiB pages only – mirrors old behaviour */
        return buddy_allocator->allocate(bytes, 0, (UInt64)-1);
    }
};

