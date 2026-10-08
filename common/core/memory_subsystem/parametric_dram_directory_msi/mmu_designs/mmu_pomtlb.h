// ============================================================================
// Part-of-Memory TLB (POM-TLB) MMU: a very large TLB in main memory
// ============================================================================
//
// Ryoo, Gulur, Song and John, "Rethinking TLB designs in virtualized
// environments: a very large part-of-memory TLB", ISCA 2017.
//
// Below the hardware TLB hierarchy sits a set-associative TLB whose entries
// live in DRAM, one per page size, each a contiguous region the OS's
// allocator hands out at start-up.  A miss in the hardware TLBs looks the
// address up in every POM-TLB at once: each lookup is one memory access of
// the set's line (through the cache hierarchy, so a hot set costs an L2
// hit), and a hit ends the translation there, with no page table walk.
// When every POM-TLB misses, the walk proceeds after the slowest lookup, and
// the resolved translation is installed in the POM-TLB of its page size.
//
// The hardware TLBs, the walker and its MSHRs, the fault loop and the TLB
// allocation cascade are the baseline MMU's; the POM-TLBs plug in through
// its two beyond-the-TLBs hooks, so mmu.page_table_walks still counts the
// walks that actually happened.
// ============================================================================

#pragma once

#include "mmu.h"
#include "tlb.h"
#include <vector>

namespace ParametricDramDirectoryMSI
{

class MemoryManagementUnitPOMTLB : public MemoryManagementUnit
{
public:
    MemoryManagementUnitPOMTLB(Core *core, MemoryManagerBase *memory_manager,
                               ShmemPerfModel *shmem_perf_model, String name,
                               MemoryManagementUnitBase *nested_mmu);
    ~MemoryManagementUnitPOMTLB();

protected:
    bool lookupBeyondTLBs(IntPtr address, IntPtr eip, bool instruction,
                          Core::lock_signal_t lock, bool modeled, bool count,
                          SubsecondTime &latency, IntPtr &ppn, int &page_size) override;
    void installBeyondTLBs(IntPtr address, IntPtr ppn, int page_size,
                           Core::lock_signal_t lock, bool count, bool instruction) override;

private:
    struct PomTLB
    {
        TLB *tlb;              // the tags and translations (statistics under pom_tlb_<i>)
        int page_size;         // the one page size it holds, in bits
        IntPtr base_address;   // where its entries live in physical memory (bytes)
    };
    std::vector<PomTLB> m_pom_tlbs;

    SimLog *m_log;

    struct
    {
        UInt64 lookups;                   // TLB misses that consulted the POM-TLBs
        UInt64 hits;                      // ... resolved there, without a walk
        UInt64 misses;                    // ... that went on to walk
        UInt64 installs;                  // translations a walk installed
        SubsecondTime software_tlb_latency;  // the lookups' memory accesses
    } m_stats;
};

}
