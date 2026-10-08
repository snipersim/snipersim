// ============================================================================
// Direct Memory Translation (DMT) MMU: a walk is one access of the leaf PTE
// ============================================================================
//
// The baseline MMU resolves a TLB miss with a radix walk: four dependent
// memory accesses (PML4 -> PDPT -> PD -> PT), some of them absorbed by the
// page walk caches.  Direct Memory Translation models a translation
// structure that is indexed directly by the virtual page number, so a TLB
// miss costs exactly one memory access: the line holding the final entry.
// The upper levels of the walk and the page walk caches are not consulted.
//
// The functional walk is still the page table's (initializeWalk), which
// decides the page size, the physical page number and whether the page
// faults; what changes is the accesses the walker is charged for.  Faults
// go through the baseline's fault loop (the generic exception-handler
// dispatch) untouched, as does everything else in the baseline MMU: the TLB
// hierarchy, the walker MSHRs, the fault pseudo-instruction, the eviction
// cascade and the per-page logs.  Only performPTW is overridden, the way
// Victima does it.
// ============================================================================

#pragma once

#include "mmu.h"

namespace ParametricDramDirectoryMSI
{

class MemoryManagementUnitDMT : public MemoryManagementUnit
{
public:
    MemoryManagementUnitDMT(Core *core, MemoryManagerBase *memory_manager,
                            ShmemPerfModel *shmem_perf_model, String name,
                            MemoryManagementUnitBase *nested_mmu);
    ~MemoryManagementUnitDMT();

    // A walk charged as the single access of its leaf entry; everything else
    // is inherited from the baseline MMU.
    PTWOutcome performPTW(IntPtr address, bool modeled, bool count, bool is_prefetch,
                          IntPtr eip, Core::lock_signal_t lock, PageTable *page_table,
                          bool restart_walk, bool instruction = false) override;

private:
    SimLog *m_log;

    // The leaf entries DMT read instead of walking: one per charged walk.
    // Registered under the MMU's name (mmu.dmt_leaf_accesses), so that a
    // run under this MMU reads one walker access per walk and nothing from
    // the page walk caches.
    UInt64 m_leaf_accesses;
    // Walks whose functional result carried no leaf (a page table that marks
    // none, or a walk that stopped early): charged as the baseline would.
    UInt64 m_fallback_walks;
};

}
