/**
 * @file fault_signature.h
 * @brief Compact fault fingerprint for memoization / replay.
 *
 * A fault signature captures the immutable and slowly-changing state
 * that determines which MimicOS code path executes and what its cost
 * is.  Signatures include epoch counters so that cache entries are
 * automatically invalidated when system state shifts.
 */
#pragma once

#include <cstdint>
#include <functional>
#include "mm/fault_phases.h"

/* ------------------------------------------------------------------ */
/*  Epoch counters (invalidation triggers)                             */
/* ------------------------------------------------------------------ */

/**
 * Global epoch state.  Each counter is bumped when the corresponding
 * subsystem's state changes materially.  Cached fault results encode
 * the epoch at creation time; a mismatch forces re-execution.
 */
struct FaultEpochs {
    uint32_t fragmentation_epoch  = 0;  /* buddy free-list shape changed */
    uint32_t page_table_epoch     = 0;  /* new PT levels allocated */
    uint32_t numa_freelist_epoch  = 0;  /* NUMA free-page balance shifted */
    uint32_t memory_pressure_epoch = 0; /* kswapd/reclaim activity */
    uint32_t scheduler_epoch      = 0;  /* runqueue balance shifted */
    uint32_t shared_region_epoch  = 0;  /* shared mapping added/removed */

    bool operator==(const FaultEpochs& o) const {
        return fragmentation_epoch == o.fragmentation_epoch &&
               page_table_epoch == o.page_table_epoch &&
               numa_freelist_epoch == o.numa_freelist_epoch &&
               memory_pressure_epoch == o.memory_pressure_epoch &&
               scheduler_epoch == o.scheduler_epoch &&
               shared_region_epoch == o.shared_region_epoch;
    }

    bool operator!=(const FaultEpochs& o) const { return !(*this == o); }
};

/* ------------------------------------------------------------------ */
/*  Fault Signature (cache key)                                        */
/* ------------------------------------------------------------------ */

struct FaultFingerprint {
    /* --- Static fault attributes --- */
    FaultMappingType mapping_type;      /* anon / file / shared */
    FaultAccessType  access_type;       /* read / write / exec */
    FaultPageSize    target_page_size;  /* 4K / 2M / 1G */

    /* --- Page-table topology --- */
    uint8_t  pt_levels_present;         /* how many PT levels already exist (0-4) */

    /* --- Allocator state (coarsened) --- */
    uint8_t  allocator_type;            /* 0=baseline, 1=reserve_thp */
    uint8_t  frag_bucket;              /* quantised fragmentation level 0-7 */
    uint8_t  numa_node;                /* target NUMA node */

    /* --- Contention / scheduling --- */
    uint8_t  thread_count_bucket;      /* 0=1, 1=2-3, 2=4-7, 3=8+ */
    uint8_t  contention_bucket;        /* 0=none, 1=low, 2=med, 3=high */

    /* --- PCP (per-CPU page set) refill classification ---
       0 = pcp hit (fast path, no buddy refill)
       1 = pcp miss (triggered __rmqueue_bulk refill)
       2 = unknown (non-anon or pre-classification)
       Faults with different pcp outcomes have materially different
       cost profiles; keeping it in the fingerprint prevents training a
       single profile on a bimodal distribution. */
    uint8_t  pcp_hit_or_miss = 2;

    /* --- Page-table install level (Phase 9 revisit) ---
       Set by the MimicOS dispatcher from the per-process installed-PMD
       / installed-PUD sets at fault entry.  Discriminates the three
       periodic anon-fault sub-populations identified in the
       fault_class_bench zoom experiment:
         0 = no install (PMD entry already present, just allocate page)
         1 = PMD install (first 4 KB fault into a fresh 2 MiB chunk)
         2 = PUD install (first 4 KB fault into a fresh 1 GiB chunk)
       Empirical real-Linux deltas on kratos20 (n=1.5M):
         0 → p50 1.39 µs  (steady)
         1 → p50 2.54 µs  (PMD install, 1.83× steady)
         2 → p50 3.72 µs  (PUD install, 2.67× steady)
       Default 0 so workloads that don't track install state degenerate
       to the legacy single-class behaviour. */
    uint8_t  pgtable_install_level = 0;

    /* --- VMA freshness (Phase 9-C) ---
       Heuristic detection of "first fault in a fresh VMA, immediately
       after the establishing mmap" — empirically 0.59× steady on
       real Linux due to kernel-cache-warmth (per-mm structures, slab
       freelists, the new VMA itself stay hot from mmap).  Computed
       from per-process last-fault-VPN distance: a fault whose VPN is
       > kFreshDistance pages from the prior fault's VPN is treated
       as fresh.  Mutually exclusive with pgtable_install_level > 0
       (the install dimension wins to avoid double classification).
         0 = fresh (warm-cache fast path)
         1 = aged  (default; long-running working set)
       Default 1 so workloads that don't track freshness degenerate
       to the legacy single-class behaviour. */
    uint8_t  vma_freshness = 1;

    /* --- Epoch snapshot (invalidation) --- */
    FaultEpochs epochs;

    /* Equality and hashing for use as unordered_map key */
    bool operator==(const FaultFingerprint& o) const {
        return mapping_type == o.mapping_type &&
               access_type == o.access_type &&
               target_page_size == o.target_page_size &&
               pt_levels_present == o.pt_levels_present &&
               allocator_type == o.allocator_type &&
               frag_bucket == o.frag_bucket &&
               numa_node == o.numa_node &&
               thread_count_bucket == o.thread_count_bucket &&
               contention_bucket == o.contention_bucket &&
               pcp_hit_or_miss == o.pcp_hit_or_miss &&
               pgtable_install_level == o.pgtable_install_level &&
               vma_freshness == o.vma_freshness &&
               epochs == o.epochs;
    }
};

struct FaultFingerprintHash {
    std::size_t operator()(const FaultFingerprint& fp) const {
        /* FNV-1a style hash over the fixed fields */
        std::size_t h = 14695981039346656037ULL;
        auto mix = [&](uint64_t v) {
            h ^= v;
            h *= 1099511628211ULL;
        };
        mix(static_cast<uint8_t>(fp.mapping_type));
        mix(static_cast<uint8_t>(fp.access_type));
        mix(static_cast<uint8_t>(fp.target_page_size));
        mix(fp.pt_levels_present);
        mix(fp.allocator_type);
        mix(fp.frag_bucket);
        mix(fp.numa_node);
        mix(fp.thread_count_bucket);
        mix(fp.contention_bucket);
        mix(fp.pcp_hit_or_miss);
        mix(fp.pgtable_install_level);
        mix(fp.vma_freshness);
        mix(fp.epochs.fragmentation_epoch);
        mix(fp.epochs.page_table_epoch);
        mix(fp.epochs.numa_freelist_epoch);
        mix(fp.epochs.memory_pressure_epoch);
        mix(fp.epochs.scheduler_epoch);
        mix(fp.epochs.shared_region_epoch);
        return h;
    }
};

/* ------------------------------------------------------------------ */
/*  Helpers to build fingerprints from runtime state                    */
/* ------------------------------------------------------------------ */

inline uint8_t quantise_fragmentation(double frag_ratio) {
    /* Map [0, 1] → bucket 0..7 */
    if (frag_ratio <= 0.0) return 0;
    if (frag_ratio >= 1.0) return 7;
    return static_cast<uint8_t>(frag_ratio * 7.999);
}

inline uint8_t quantise_thread_count(int n) {
    if (n <= 1) return 0;
    if (n <= 3) return 1;
    if (n <= 7) return 2;
    return 3;
}
