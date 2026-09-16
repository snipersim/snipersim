/**
 * @file fault_cache.h
 * @brief Memoisation cache for fault-path decisions and replay.
 *
 * Caches the structural outcome of a page-fault handler execution
 * (which branch path was taken, which sub-phases executed, allocator
 * subroutine choice, phase-cost model coefficients) so that repeated
 * faults with the same fingerprint can be replayed cheaply.
 *
 * Three replay modes are supported:
 *   - Detailed:           full MimicOS handler execution
 *   - Structured replay:  skip branch-heavy control flow, materialise state
 *   - Analytical replay:  charge calibrated cost, essential state updates only
 *
 * Cache validity is guarded by epoch counters (see fault_signature.h).
 */
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <mutex>

#include "mm/fault_signature.h"
#include "mm/fault_phases.h"

/* ------------------------------------------------------------------ */
/*  Replay mode                                                        */
/* ------------------------------------------------------------------ */

enum class FaultReplayMode : uint8_t {
    DETAILED    = 0,  /* full MimicOS handler */
    STRUCTURED  = 1,  /* skip branches, materialise state */
    ANALYTICAL  = 2,  /* charge calibrated cost only */
};

/* ------------------------------------------------------------------ */
/*  Cached fault artifact                                              */
/* ------------------------------------------------------------------ */

struct CachedFaultResult {
    /* --- Branch path taken --- */
    bool used_buddy_allocator    = true;
    bool used_reserve_thp        = false;
    bool promoted_to_2mb         = false;
    bool needed_pt_page_alloc    = false;
    uint8_t pt_levels_touched    = 0;

    /* --- Phase costs (calibrated, ns) --- */
    uint64_t phase_costs[kNumFaultPhases] = {};
    uint64_t total_cost_ns = 0;

    /* --- Side effects to materialise --- */
    /* The allocator's state changes (number of pages consumed, etc.)
       are always materialised even in analytical mode.  Only the
       control-flow path discovery is skipped. */
    uint64_t pages_consumed     = 1;  /* data pages allocated */
    uint64_t pt_pages_consumed  = 0;  /* PT pages allocated */
    FaultPageSize result_page_size = FaultPageSize::PAGE_4K;

    /* --- Sampling bookkeeping --- */
    uint32_t hit_count      = 0;  /* times this entry was reused */
    uint32_t detailed_count = 0;  /* times detailed mode ran for this fp */
    uint32_t last_detailed_at = 0; /* fault serial when last detailed */
};

/* ------------------------------------------------------------------ */
/*  Sampling policy                                                    */
/* ------------------------------------------------------------------ */

struct FaultSamplingPolicy {
    /* First N faults of a new fingerprint: always detailed */
    uint32_t warmup_count = 16;

    /* After warmup: 1 in K faults are detailed */
    uint32_t sample_interval = 64;

    /* Back off to this interval for very stable signatures */
    uint32_t stable_interval = 256;

    /* Stability threshold: if last N detailed runs agree within
       this tolerance, consider stable */
    uint32_t stable_after = 32;
    double   stable_tolerance = 0.05;  /* 5% */

    /* Force detailed after any epoch change */
    bool force_on_epoch_change = true;
};

/* ------------------------------------------------------------------ */
/*  Fault replay cache                                                 */
/* ------------------------------------------------------------------ */

class FaultCache {
public:
    FaultCache() : enabled_(false), global_fault_serial_(0) {}

    void enable()  { enabled_ = true; }
    void disable() { enabled_ = false; }
    bool is_enabled() const { return enabled_; }

    void set_sampling_policy(const FaultSamplingPolicy& p) { policy_ = p; }

    /**
     * Look up a fingerprint.  Returns:
     *   - nullptr if not cached (caller must execute detailed path)
     *   - pointer to cached result if available
     *
     * Also determines the recommended replay mode via @p mode_out.
     */
    CachedFaultResult* lookup(const FaultFingerprint& fp,
                              FaultReplayMode& mode_out) {
        if (!enabled_) {
            mode_out = FaultReplayMode::DETAILED;
            return nullptr;
        }

        uint32_t serial = global_fault_serial_++;

        auto it = cache_.find(fp);
        if (it == cache_.end()) {
            /* Cold miss */
            mode_out = FaultReplayMode::DETAILED;
            stats_.cold_misses++;
            return nullptr;
        }

        CachedFaultResult& entry = it->second;

        /* Epoch check: if epochs differ, the entry is stale */
        /* (Epochs are embedded in the fingerprint, so a stale entry
            simply won't match.  This check is redundant but explicit.) */

        /* Decide replay mode */
        if (entry.detailed_count < policy_.warmup_count) {
            mode_out = FaultReplayMode::DETAILED;
            stats_.warmup_detailed++;
        } else if ((entry.hit_count % policy_.sample_interval) == 0) {
            mode_out = FaultReplayMode::DETAILED;
            stats_.sample_detailed++;
        } else if (entry.hit_count > policy_.stable_after &&
                   entry.detailed_count >= policy_.warmup_count) {
            mode_out = FaultReplayMode::ANALYTICAL;
            stats_.analytical_replays++;
        } else {
            mode_out = FaultReplayMode::STRUCTURED;
            stats_.structured_replays++;
        }

        entry.hit_count++;
        entry.last_detailed_at = (mode_out == FaultReplayMode::DETAILED) ? serial : entry.last_detailed_at;
        if (mode_out == FaultReplayMode::DETAILED)
            entry.detailed_count++;

        return &entry;
    }

    /**
     * Insert or update a cache entry after a detailed execution.
     *
     * If the fingerprint already exists, preserve sampling bookkeeping
     * (hit_count, detailed_count, last_detailed_at) and only update the
     * result data (costs, page size, etc).  This lets warmup actually
     * complete — without this, every record_detailed() would reset the
     * counters and warmup would never progress.
     */
    void insert(const FaultFingerprint& fp, const CachedFaultResult& result) {
        if (!enabled_) return;
        auto it = cache_.find(fp);
        if (it == cache_.end()) {
            cache_[fp] = result;
        } else {
            CachedFaultResult& existing = it->second;
            uint32_t saved_hit      = existing.hit_count;
            uint32_t saved_detailed = existing.detailed_count;
            uint32_t saved_last     = existing.last_detailed_at;
            existing = result;
            existing.hit_count        = saved_hit;
            existing.detailed_count   = saved_detailed;
            existing.last_detailed_at = saved_last;
        }
        stats_.insertions++;
    }

    /**
     * Invalidate all entries (e.g. after a major state change).
     */
    void invalidate_all() {
        cache_.clear();
        stats_.full_invalidations++;
    }

    /**
     * Get current epochs reference for the caller to bump.
     */
    FaultEpochs& epochs() { return current_epochs_; }
    const FaultEpochs& epochs() const { return current_epochs_; }

    /* ---- Statistics ---- */
    struct Stats {
        uint64_t cold_misses         = 0;
        uint64_t warmup_detailed     = 0;
        uint64_t sample_detailed     = 0;
        uint64_t structured_replays  = 0;
        uint64_t analytical_replays  = 0;
        uint64_t insertions          = 0;
        uint64_t full_invalidations  = 0;

        uint64_t total_lookups() const {
            return cold_misses + warmup_detailed + sample_detailed +
                   structured_replays + analytical_replays;
        }

        double hit_rate() const {
            uint64_t total = total_lookups();
            if (total == 0) return 0;
            return (double)(structured_replays + analytical_replays) / total;
        }
    };

    const Stats& stats() const { return stats_; }

    size_t size() const { return cache_.size(); }

    /**
     * Dump cache statistics to a CSV file.
     */
    void dump_stats(const std::string& filepath) const {
        std::ofstream f(filepath);
        if (!f.is_open()) return;

        f << "metric,value\n";
        f << "cache_entries," << cache_.size() << "\n";
        f << "total_lookups," << stats_.total_lookups() << "\n";
        f << "cold_misses," << stats_.cold_misses << "\n";
        f << "warmup_detailed," << stats_.warmup_detailed << "\n";
        f << "sample_detailed," << stats_.sample_detailed << "\n";
        f << "structured_replays," << stats_.structured_replays << "\n";
        f << "analytical_replays," << stats_.analytical_replays << "\n";
        f << "insertions," << stats_.insertions << "\n";
        f << "full_invalidations," << stats_.full_invalidations << "\n";
        f << "hit_rate," << stats_.hit_rate() << "\n";
    }

private:
    bool enabled_;
    FaultSamplingPolicy policy_;
    FaultEpochs current_epochs_;
    std::atomic<uint32_t> global_fault_serial_;
    Stats stats_;

    std::unordered_map<FaultFingerprint, CachedFaultResult,
                       FaultFingerprintHash> cache_;
};
