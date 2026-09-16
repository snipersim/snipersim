/**
 * @file fault_replay.h
 * @brief Fast-path fault replay engine for MimicOS.
 *
 * Orchestrates the three replay modes (detailed, structured, analytical)
 * and manages the interaction between the fault cache, epoch tracking,
 * and the actual MimicOS allocator/page-table subsystems.
 *
 * The replay engine wraps around the existing poll_for_signal() allocator
 * calls, deciding per-fault whether to execute the full path or replay.
 */
#pragma once

#include <cstdint>
#include <string>
#include <fstream>
#include <iostream>

#include "mm/fault_cache.h"
#include "mm/fault_signature.h"
#include "mm/fault_phases.h"

/* Forward declarations */
class PhysicalMemoryAllocator;

/* ------------------------------------------------------------------ */
/*  Replay configuration (loaded from INI)                             */
/* ------------------------------------------------------------------ */

struct FaultReplayConfig {
    bool enabled = false;

    /* Allow specific modes to be disabled for A/B testing */
    bool allow_structured_replay = true;
    bool allow_analytical_replay = true;

    /* Force all faults to detailed mode (for regression baseline) */
    bool force_detailed = false;

    /* Sampling policy tunables */
    uint32_t warmup_count    = 16;
    uint32_t sample_interval = 64;
    uint32_t stable_interval = 256;
    uint32_t stable_after    = 32;
    double   stable_tolerance = 0.05;

    /* Epoch sensitivity */
    bool invalidate_on_fragmentation_change = true;
    bool invalidate_on_pt_growth = true;
    bool invalidate_on_pressure_change = true;
};

/* ------------------------------------------------------------------ */
/*  Replay result (returned to caller)                                 */
/* ------------------------------------------------------------------ */

struct FaultReplayResult {
    FaultReplayMode mode_used;
    uint64_t charged_cost_ns;    /* latency to charge for this fault */
    uint64_t pa;                 /* physical address (always materialised) */
    uint64_t page_size;          /* 12 or 21 */
    bool     cache_hit;
};

/* ------------------------------------------------------------------ */
/*  Fault Replay Engine                                                */
/* ------------------------------------------------------------------ */

class FaultReplayEngine {
public:
    FaultReplayEngine()
        : config_(), allocator_(nullptr), pt_pages_allocated_(0)
    {}

    void configure(const FaultReplayConfig& cfg) {
        config_ = cfg;
        if (cfg.enabled) {
            cache_.enable();
            FaultSamplingPolicy pol;
            pol.warmup_count    = cfg.warmup_count;
            pol.sample_interval = cfg.sample_interval;
            pol.stable_interval = cfg.stable_interval;
            pol.stable_after    = cfg.stable_after;
            pol.stable_tolerance = cfg.stable_tolerance;
            cache_.set_sampling_policy(pol);
            std::cout << "[MimicOS] FaultReplayEngine enabled (warmup="
                      << cfg.warmup_count << " sample_interval="
                      << cfg.sample_interval << ")" << std::endl;
        }
    }

    void bind_allocator(PhysicalMemoryAllocator* alloc) {
        allocator_ = alloc;
    }

    bool is_enabled() const { return config_.enabled && !config_.force_detailed; }

    /**
     * Build a fingerprint for the current fault from runtime state.
     */
    FaultFingerprint build_fingerprint(
        uint64_t /* va */,
        FaultMappingType mapping,
        FaultAccessType access,
        FaultPageSize target_psize,
        uint8_t pt_levels_present,
        uint8_t allocator_type,
        double frag_ratio,
        uint8_t numa_node,
        int thread_count,
        int contention_level) const
    {
        FaultFingerprint fp;
        fp.mapping_type       = mapping;
        fp.access_type        = access;
        fp.target_page_size   = target_psize;
        fp.pt_levels_present  = pt_levels_present;
        fp.allocator_type     = allocator_type;
        fp.frag_bucket        = quantise_fragmentation(frag_ratio);
        fp.numa_node          = numa_node;
        fp.thread_count_bucket = quantise_thread_count(thread_count);
        fp.contention_bucket  = static_cast<uint8_t>(contention_level);
        fp.epochs             = cache_.epochs();  /* snapshot current epochs */
        return fp;
    }

    /**
     * Attempt to replay a fault from cache.
     *
     * Returns true if the fault was handled via replay (structured or
     * analytical).  Returns false if a detailed execution is needed.
     *
     * When returning true, @p result is populated with the charged cost
     * and the physical address.  The caller still needs to materialise
     * the actual allocation (the replay engine does NOT call the allocator
     * in analytical mode — the caller decides whether to do so).
     *
     * When returning false, the caller should execute the full detailed
     * path and then call record_detailed() with the outcome.
     */
    bool try_replay(const FaultFingerprint& fp, FaultReplayResult& result) {
        if (!is_enabled()) return false;

        FaultReplayMode mode;
        CachedFaultResult* cached = cache_.lookup(fp, mode);

        if (config_.force_detailed || mode == FaultReplayMode::DETAILED) {
            return false;
        }

        if (!cached) return false;

        /* Clamp mode based on config */
        if (mode == FaultReplayMode::ANALYTICAL && !config_.allow_analytical_replay)
            mode = FaultReplayMode::STRUCTURED;
        if (mode == FaultReplayMode::STRUCTURED && !config_.allow_structured_replay)
            return false;

        /* Fill result from cache */
        result.mode_used = mode;
        result.charged_cost_ns = cached->total_cost_ns;
        result.page_size = (cached->result_page_size == FaultPageSize::PAGE_2M) ? 21 : 12;
        result.cache_hit = true;

        /* In structured mode, we still need to allocate pages.
           In analytical mode, we skip even that and just charge cost.
           The caller decides based on result.mode_used. */

        return true;
    }

    /**
     * Record the result of a detailed fault execution into the cache.
     */
    void record_detailed(const FaultFingerprint& fp,
                         const FaultPhaseTimings& timings,
                         uint64_t /* pa */,
                         uint64_t page_size_bits,
                         int num_pt_pages_allocated) {
        if (!config_.enabled) return;

        CachedFaultResult entry;
        entry.needed_pt_page_alloc = (num_pt_pages_allocated > 0);
        entry.pt_levels_touched = static_cast<uint8_t>(num_pt_pages_allocated);
        entry.promoted_to_2mb = (page_size_bits == 21);
        entry.result_page_size = (page_size_bits == 21) ?
            FaultPageSize::PAGE_2M : FaultPageSize::PAGE_4K;
        entry.pages_consumed = 1;
        entry.pt_pages_consumed = num_pt_pages_allocated;
        entry.total_cost_ns = timings.total_duration();

        for (int i = 0; i < kNumFaultPhases; i++) {
            entry.phase_costs[i] = timings.phase_duration(static_cast<FaultPhase>(i));
        }

        cache_.insert(fp, entry);
    }

    /* ---- Epoch management ---- */

    void bump_fragmentation_epoch() {
        if (config_.invalidate_on_fragmentation_change)
            cache_.epochs().fragmentation_epoch++;
    }

    void bump_page_table_epoch() {
        if (config_.invalidate_on_pt_growth)
            cache_.epochs().page_table_epoch++;
    }

    void bump_pressure_epoch() {
        if (config_.invalidate_on_pressure_change)
            cache_.epochs().memory_pressure_epoch++;
    }

    void bump_scheduler_epoch() {
        cache_.epochs().scheduler_epoch++;
    }

    void bump_shared_region_epoch() {
        cache_.epochs().shared_region_epoch++;
    }

    /* ---- Statistics & diagnostics ---- */

    const FaultCache::Stats& cache_stats() const { return cache_.stats(); }

    void dump_stats(const std::string& filepath) const {
        cache_.dump_stats(filepath);

        /* Also dump replay engine level stats */
        std::string engine_path = filepath;
        auto pos = engine_path.rfind('.');
        if (pos != std::string::npos)
            engine_path.insert(pos, "_engine");
        else
            engine_path += "_engine.csv";

        std::ofstream f(engine_path);
        if (!f.is_open()) return;
        f << "metric,value\n";
        f << "enabled," << config_.enabled << "\n";
        f << "force_detailed," << config_.force_detailed << "\n";
        f << "cache_size," << cache_.size() << "\n";
        f << "cache_hit_rate," << cache_.stats().hit_rate() << "\n";
        f << "frag_epoch," << cache_.epochs().fragmentation_epoch << "\n";
        f << "pt_epoch," << cache_.epochs().page_table_epoch << "\n";
        f << "pressure_epoch," << cache_.epochs().memory_pressure_epoch << "\n";
        f << "sched_epoch," << cache_.epochs().scheduler_epoch << "\n";
        f << "shm_epoch," << cache_.epochs().shared_region_epoch << "\n";
    }

private:
    FaultReplayConfig config_;
    FaultCache cache_;
    PhysicalMemoryAllocator* allocator_;
    uint64_t pt_pages_allocated_;
};
