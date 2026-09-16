#pragma once
/*------------------------------------------------------------------------------
 *  LinuxBuddyAnonAllocator
 *
 *  A new allocator that mimics the Linux kernel's anonymous-page allocation
 *  fast path more closely than the existing baseline / reserve_thp allocators.
 *
 *  Linux uses a per-CPU pageset (see mm/page_alloc.c free_pcppages_bulk +
 *  rmqueue_pcplist) to satisfy the common 4-KB allocation in ~300-1500 cycles
 *  without taking the global zone lock.  Cold path falls through to the buddy
 *  allocator.  This matches the cost decomposition we measured with eBPF on
 *  real Linux for anon-write minor faults.
 *
 *  This allocator is ADDITIVE — it does not modify baseline.h or reserve_thp.h.
 *  Select it via INI:  [allocator] memory_allocator = linux_buddy_anon
 *----------------------------------------------------------------------------*/
#include "debug_config.h"
#include "memory_management/physical_memory_allocators/physical_memory_allocator.h"
#include "templates_traits_config.h"
#include "memory_management/physical_memory_allocators/buddy_policy_traits.h"
#include "memory_management/physical_memory_allocators/buddy.h"

#include <vector>
#include <deque>
#include <cassert>
#include <cstring>

/* Macro-gated -O0 (see buddy.h for rationale). */
#ifdef MIMICOS_CALIBRATION_O0
#pragma GCC push_options
#pragma GCC optimize ("O0")
#endif

template <typename Policy>
class LinuxBuddyAnonAllocator : public PhysicalMemoryAllocator, private Policy
{
    using BuddyPolicy = typename BuddyPolicyFor<Policy>::type;
    using BuddyType   = Buddy<BuddyPolicy>;

    /*------------------------  shared statistics  ------------------------------*/
    struct Stats {
        UInt64 total_allocations  = 0;
        UInt64 pcp_hits           = 0;   /* served by per-CPU pageset (fast)   */
        UInt64 pcp_misses         = 0;   /* refilled pageset, then satisfied   */
        UInt64 pcp_refills        = 0;
    } stats;

    /* Linux per-CPU pageset: a ring of pre-allocated 4-KB PFNs per core.
       When empty, we refill from buddy in a batch, mimicking
       free_pcppages_bulk / rmqueue_pcplist semantics. */
    struct PerCpuPageset {
        std::deque<UInt64> free_pfns;       /* pfn cache (LIFO ideally) */
        UInt64             low_watermark  = 16;   /* trigger refill below this */
        UInt64             refill_batch   = 32;   /* pages to grab per refill */
    };

    static constexpr int  MAX_CORES = 64;
    PerCpuPageset         pcps[MAX_CORES];

public:
    Stats& getStats() { return stats; }

    LinuxBuddyAnonAllocator(String name,
                            int    memory_size,
                            int    max_order,
                            int    kernel_size,
                            String frag_type)
        : PhysicalMemoryAllocator(name, memory_size, kernel_size)
    {
        Policy::on_init(name, memory_size, kernel_size, this);

        static_assert(is_complete<BuddyPolicyFor<Policy>>::value,
            "BuddyPolicyFor<Policy> is incomplete");

        buddy_allocator = new BuddyType(memory_size, max_order, kernel_size, frag_type);
    }

    ~LinuxBuddyAnonAllocator() { delete buddy_allocator; }

    /*--------------------------- public interface ------------------------------*/
#ifdef MIMICOS_CALIBRATION_O0
    __attribute__((noinline, optimize("O0")))
#endif
    std::pair<UInt64, UInt64> allocate(UInt64 bytes,
                                       UInt64 address               = 0,
                                       UInt64 core_id               = (UInt64)-1,
                                       bool   is_pagetable_alloc    = false,
                                       bool   is_instruction_alloc  = false) override
    {
        stats.total_allocations++;

        if (is_pagetable_alloc) {
            UInt64 phys = handle_page_table_allocations(bytes);
            return {phys, 12};
        }
        if (is_instruction_alloc) {
            return this->allocateInstruction(bytes);
        }

        /* Fast path: try per-CPU pageset first.  This mirrors Linux's
           rmqueue_pcplist for order-0 pages. */
        int cpu = (core_id == (UInt64)-1) ? 0 : (int)(core_id % MAX_CORES);
        PerCpuPageset& pcp = pcps[cpu];

        if (pcp.free_pfns.empty()) {
            /* Cold path: refill pageset from buddy in ONE batch call.
               Mirrors Linux's __rmqueue_pcplist -> rmqueue_bulk — one
               zone_lock acquire, N cheap list-pops plus an amortised
               expand() split chain.  Previously we looped allocate(4096)
               N times, which paid per-call overhead (ceil/log2, full
               order-list scan, function-call frame) on every page.
               Sniper-simulated that at ~80 000 cyc per 32-page refill;
               real Linux measures ~2 500 cyc for the whole batch.  The
               batch path in Buddy::allocate_batch_order0 pulls the
               per-refill cost ~30× closer to real Linux. */
            stats.pcp_misses++;
            stats.pcp_refills++;
            this->last_alloc_fastpath = 0;  /* Phase 8: pcp miss */

            std::vector<UInt64> batch;
            buddy_allocator->allocate_batch_order0(
                pcp.refill_batch, batch, core_id);
            for (UInt64 pfn : batch) pcp.free_pfns.push_back(pfn);

            if (pcp.free_pfns.empty()) {
                /* Buddy exhausted */
                return {(UInt64)-1, 12};
            }
        } else {
            stats.pcp_hits++;
            this->last_alloc_fastpath = 1;  /* Phase 8: pcp hit */
        }

        UInt64 pfn = pcp.free_pfns.back();
        pcp.free_pfns.pop_back();
        return {pfn, 12};
    }

    std::vector<Range> allocate_ranges(IntPtr /*start_va*/,
                                       IntPtr /*end_va*/,
                                       int    /*app_id*/) override
    {
        return {};
    }

    UInt64 givePageFast(UInt64 bytes,
                        UInt64 address = 0,
                        UInt64 core_id = (UInt64)-1) override
    {
        return buddy_allocator->allocate(bytes, address, core_id);
    }

    void deallocate(UInt64 /*region_begin*/,
                    UInt64 /*core_id*/ = (UInt64)-1) override
    {
        /* For now, mimicking Linux pcp-free is overkill — pages stay leaked
           in the pcp until the run ends.  The MimicOS workloads we care
           about are fault-heavy anon-allocation traces, not free-heavy. */
    }

    void fragment_memory(double target_fragmentation)
    {
        buddy_allocator->fragmentMemory(target_fragmentation);
    }

private:
    BuddyType* buddy_allocator {nullptr};

    UInt64 handle_page_table_allocations(UInt64 bytes)
    {
        return buddy_allocator->allocate(bytes, 0, (UInt64)-1);
    }
};

#ifdef MIMICOS_CALIBRATION_O0
#pragma GCC pop_options
#endif
