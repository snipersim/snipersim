#pragma once

/*
 * Utopia allocator with implicit 2MB coalescing (utopia_hash_coalesce.h upstream)
 *
 * A 4KB RestSeg with a way-major frame layout (utopia.h) has a property the
 * plain allocator does not use: the 512 pages of a 2MB-aligned virtual
 * region map to 512 consecutive sets, and placing them all in the same way
 * puts them in 512 consecutive frames -- one contiguous, aligned 2MB page.
 * Before the ordinary hierarchy, this allocator checks whether some way is
 * free in every one of those 512 sets (a bitwise AND of the sets' free-way
 * bitmaps, with early exit) and, if so, places the whole region at once,
 * records it in the RestSeg's 2MB radix way table and returns a 2MB page:
 * the coalescing promotion.  A later fault in a region already coalesced is
 * a coalescing hit.  The hierarchy is then the plain one: RestSeg-2MB,
 * FlexSeg-THP, RestSeg-4KB, FlexSeg-4KB.
 *
 * The MMU side is mmu_utopia_coalesce.*: the RestSeg walk checks the 2MB
 * table (three lines) before the per-page one (four).  The class derives
 * from UtopiaAllocator on the same policy, so the Utopia MMU and fault
 * handler see it as the Utopia allocator it is.
 */

#include "memory_management/physical_memory_allocators/utopia.h"

template <typename Policy>
class UtopiaHashCoalesce : public UtopiaAllocator<Policy>
{
    typedef UtopiaAllocator<Policy> Base;

public:
    UtopiaHashCoalesce(String name, int memory_size, int max_order, int kernel_size, String frag_type)
        : Base(name, memory_size, max_order, kernel_size, frag_type)
    {
        Policy::register_counter(name, "coalescing_promotions", &this->stats.coalescing_promotions);
        Policy::register_counter(name, "coalescing_hits", &this->stats.coalescing_hits);
        for (int i = 0; i < this->num_restsegs; i++)
        {
            RestSeg *rs = this->restsegs[i].get();
            String p = "restseg" + itostr(i) + "_";
            Policy::register_counter(name, p + "coalesced_2mb_lookups", rs->getCoalesced2MBLookupsPtr());
            Policy::register_counter(name, p + "coalesced_2mb_hits", rs->getCoalesced2MBHitsPtr());
        }
    }

    std::pair<UInt64, UInt64> allocate(UInt64 size, UInt64 address = 0, UInt64 core_id = -1,
                                       bool is_pagetable_allocation = false,
                                       bool is_instruction_allocation = false) override
    {
        if (!is_pagetable_allocation && !is_instruction_allocation && size == 4096 && this->restseg_4kb_index >= 0)
        {
            RestSeg *seg = this->restsegs[this->restseg_4kb_index].get();
            int app_id = Policy::app_id_for_core(core_id);
            UInt64 aligned_address = address & ~((2ULL << 20) - 1);
            UInt64 start_vpn = aligned_address >> 12;

            // The region is coalesced already: its 2MB page
            int existing_way = seg->lookupCoalesced2MBWay(start_vpn, app_id);
            if (existing_way >= 0)
            {
                this->stats.total_allocations++;
                this->stats.coalescing_hits++;
                this->stats.restseg_allocations++;
                this->last_in_restseg = true;
                this->last_caused_eviction = false;
                this->last_evicted_address = 0;
                IntPtr base_ppn = seg->coalescedRegionPPN(aligned_address, existing_way);
                this->log_debug("[L0] coalesced region hit: VA=", SimLog::hex(address), " -> PPN=", base_ppn, " way=", existing_way);
                return std::make_pair((UInt64)base_ppn, 21ULL);
            }
            // A way free across the region's 512 sets: place the region in it
            int common_way = seg->findCommonFreeWay(start_vpn);
            if (common_way >= 0)
            {
                for (int i = 0; i < 512; i++)
                    seg->tryInsertAtWay(aligned_address + ((UInt64)i << 12), app_id, common_way, false /* no per-page radix entry */);
                seg->setCoalesced2MBWay(start_vpn, app_id, common_way);
                this->stats.total_allocations++;
                this->stats.restseg_2mb_allocations++;
                this->stats.restseg_allocations++;
                this->stats.coalescing_promotions++;
                this->last_in_restseg = true;
                this->last_caused_eviction = false;
                this->last_evicted_address = 0;
                IntPtr base_ppn = seg->coalescedRegionPPN(aligned_address, common_way);
                this->log_debug("[L0] coalesced 2MB region: VA=", SimLog::hex(address), " -> PPN=", base_ppn, " way=", common_way);
                return std::make_pair((UInt64)base_ppn, 21ULL);
            }
        }
        return Base::allocate(size, address, core_id, is_pagetable_allocation, is_instruction_allocation);
    }
};

// The Sniper instantiation: on the Utopia policy, so that it is a Utopia
// (UtopiaAllocator<Sniper::Utopia::MetricsPolicy>) to the MMU and the fault
// handler, which take the allocator by that type
using UtopiaCoalesce = UtopiaHashCoalesce<Sniper::Utopia::MetricsPolicy>;
