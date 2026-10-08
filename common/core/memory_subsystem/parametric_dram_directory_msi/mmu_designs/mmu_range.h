// ============================================================================
// Range MMU: Redundant Memory Mappings [Karakostas et al., ISCA 2015]
// ============================================================================
//
// Alongside the page table, the OS keeps a range table: every VMA the
// eager-paging allocator laid out as a few physically contiguous ranges is
// one entry per range (base, end, frame of the base).  A hardware TLB miss
// looks the address up in the Range Lookaside Buffer; an RLB miss walks the
// range table (a B-tree in memory, charged node by node through the cache
// hierarchy) and fills the RLB; a range hit resolves the translation
// without a page table walk, since PPN = frame + (VPN - base VPN).  Only an
// address no range covers walks the page table.
//
// The hardware TLBs, the walker and the fault loop are the baseline MMU's;
// the RLB and the range walk plug in through its beyond-the-TLBs hooks, as
// the POM-TLB does: looked up after the TLB miss, before a walker is taken.
// (RMM overlaps the RLB lookup with the L2 TLB's; here the RLB is charged
// after the last TLB level missed.)  The range table is filled by the
// eager-paging exception handler at fault time, so the MMU installs
// nothing after a walk.
// ============================================================================

#pragma once

#include "mmu.h"
#include "rangelb.h"

namespace ParametricDramDirectoryMSI
{

class RangeMMU : public MemoryManagementUnit
{
public:
    RangeMMU(Core *core, MemoryManagerBase *memory_manager,
             ShmemPerfModel *shmem_perf_model, String name,
             MemoryManagementUnitBase *nested_mmu);
    ~RangeMMU();

protected:
    bool lookupBeyondTLBs(IntPtr address, IntPtr eip, bool instruction,
                          Core::lock_signal_t lock, bool modeled, bool count,
                          SubsecondTime &latency, IntPtr &ppn, int &page_size) override;

private:
    RLB *m_rlb;
    bool m_rlb_enabled;
    SimLog *m_log;

    struct
    {
        UInt64 requests_resolved_by_rlb;             // TLB misses the RLB answered
        UInt64 requests_resolved_by_range_table;     // ... the range table answered, filling the RLB
        UInt64 range_table_misses;                   // ... no range covered: a page table walk
        SubsecondTime requests_resolved_by_rlb_latency;
        SubsecondTime total_range_walk_latency;      // the range table's memory accesses
    } m_stats;
};

}
