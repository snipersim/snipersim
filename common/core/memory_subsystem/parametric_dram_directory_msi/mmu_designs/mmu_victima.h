// ============================================================================
// Victima MMU — leaf-probe over real cache hierarchy
// Based on: "Victima: Drastically Increasing Address Translation Reach by
//            Leveraging Underutilized Cache Resources", MICRO 2023
// ============================================================================
//
// Inherits the default MMU's translation infrastructure (TLB hierarchy,
// MSHR, fault handling, time advance, eviction cascade, pseudo-instruction
// for fault latency).  Overrides performPTW only:
//
//   * On L2 TLB miss, do a single leaf-only accessCache tagged
//     VICTIMA_TLB_BLOCK.  Tag-as-VICTIMA gives the leaf line sticky SRRIP
//     priority via the existing isPageTableBlock() pathway in cache_set_srrip.
//   * If the leaf line is present in the cache hierarchy (L1/L2/LLC), skip
//     the upper-level walk: return PTWOutcome with latency = leaf access.
//   * Else fall back to a normal multi-level walk (calculatePTWCycles) for
//     the upper levels and add the leaf latency.
//
// PTW Cost Predictor: not gated.  Always-insert mode (every PTW retags
// the leaf VICTIMA).  Eviction-triggered background fetch: TODO.
// ============================================================================

#pragma once

#include "mmu.h"
#include "spec_engine_base.h"

namespace ParametricDramDirectoryMSI
{

class MemoryManagementUnitVictima : public MemoryManagementUnit
{
public:
    MemoryManagementUnitVictima(Core* core, MemoryManagerBase* memory_manager,
                                ShmemPerfModel* shmem_perf_model, String name,
                                MemoryManagementUnitBase* nested_mmu);

    // Override only the page-table-walk path; everything else is inherited.
    PTWOutcome performPTW(IntPtr address, bool modeled, bool count, bool is_prefetch,
                          IntPtr eip, Core::lock_signal_t lock, PageTable* page_table,
                          bool restart_walk, bool instruction = false) override;

protected:
    // Eviction-triggered background fetch: bring the evicted VPN's leaf line back
    // into the cache hierarchy and tag it VICTIMA_TLB_BLOCK.  Latency is not
    // charged to the demand path.
    void onTLBLevelEviction(int level, IntPtr evicted_address, int evicted_page_size,
                            IntPtr evicted_ppn, IntPtr eip, Core::lock_signal_t lock,
                            bool instruction) override;

private:
    bool m_victima_enabled;
    bool m_bg_fetch_enabled;
    bool m_leaf_probe_enabled;
    SpecEngineBase* m_spec_engine;
    UInt64 m_spec_invocations;
    UInt64 m_spec_correct;
    UInt64 m_victima_leaf_probes;
    UInt64 m_victima_leaf_hits;
    UInt64 m_victima_leaf_hits_l1;
    UInt64 m_victima_leaf_hits_l2;
    UInt64 m_victima_leaf_hits_llc;
    UInt64 m_victima_leaf_misses;
    UInt64 m_victima_bg_fetches;
    SubsecondTime m_victima_leaf_probe_latency;
    SubsecondTime m_victima_upper_walk_latency;

    // Parallel-probe model: tag-only peek at L2 and NUCA in parallel, with
    // L2-TLB-lookup overlap.  No fill, no DRAM round-trip on miss, no cache
    // pollution from speculation.  Effective per-hit-where charges already
    // bake in the L2-TLB overlap (raw latency - overlap):
    //   L2 hit  -> 0 cyc  (12 cyc L2 data - 12 cyc L2 TLB overlap)
    //   NUCA hit-> 8 cyc  (20 cyc NUCA data - 12 cyc L2 TLB overlap)
    //   miss    -> 3 cyc  (15 cyc NUCA tag - 12 cyc L2 TLB overlap)
    bool m_parallel_probe_enabled;
    UInt32 m_pp_eff_l2_cyc;
    UInt32 m_pp_eff_nuca_cyc;
    UInt32 m_pp_eff_miss_cyc;
    UInt64 m_victima_pp_l2_hits;
    UInt64 m_victima_pp_nuca_hits;
    UInt64 m_victima_pp_misses;
    SubsecondTime victimaParallelProbe(IntPtr leaf_addr, HitWhere::where_t& out_hw, bool count);
};

}  // namespace ParametricDramDirectoryMSI
