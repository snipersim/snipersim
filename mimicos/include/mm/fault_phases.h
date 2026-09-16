/**
 * @file fault_phases.h
 * @brief Fault-phase enumeration and per-phase instrumentation for MimicOS.
 *
 * Decomposes minor page-fault handling into named phases with cycle-level
 * timing, histograms, and classification.  Used for calibration against
 * real-hardware eBPF/ftrace measurements and for the fast-replay engine.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <array>
#include <vector>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <mutex>
#include <fstream>

/* ------------------------------------------------------------------ */
/*  Phase enumeration                                                  */
/* ------------------------------------------------------------------ */

enum class FaultPhase : uint8_t {
    FAULT_TRAP_ENTRY   = 0,
    FAULT_VMA_LOOKUP   = 1,
    FAULT_PT_ALLOC     = 2,
    FAULT_PHYS_ALLOC   = 3,
    FAULT_ZERO_FILL    = 4,
    FAULT_PTE_INSTALL  = 5,
    FAULT_TLB_SYNC     = 6,
    FAULT_BOOKKEEPING  = 7,
    FAULT_EXIT         = 8,
    NUM_PHASES
};

static constexpr int kNumFaultPhases = static_cast<int>(FaultPhase::NUM_PHASES);

inline const char* fault_phase_name(FaultPhase p) {
    static const char* names[] = {
        "trap_entry", "vma_lookup", "pt_alloc", "phys_alloc",
        "zero_fill",  "pte_install", "tlb_sync", "bookkeeping", "exit",
    };
    int idx = static_cast<int>(p);
    return (idx >= 0 && idx < kNumFaultPhases) ? names[idx] : "unknown";
}

/* ------------------------------------------------------------------ */
/*  Fault classification                                               */
/* ------------------------------------------------------------------ */

enum class FaultMappingType : uint8_t {
    ANON = 0,
    FILE_BACKED,
    SHARED,
    UNKNOWN,
};

enum class FaultAccessType : uint8_t {
    READ = 0,
    WRITE,
    EXEC,
    UNKNOWN,
};

enum class FaultPageSize : uint8_t {
    PAGE_4K = 0,
    PAGE_2M,
    PAGE_1G,
};

/**
 * Per-fault classification record.  Populated at the beginning of a
 * fault and used as a key for memoization / replay.
 */
struct FaultClass {
    FaultMappingType mapping   = FaultMappingType::UNKNOWN;
    FaultAccessType  access    = FaultAccessType::UNKNOWN;
    FaultPageSize    page_size = FaultPageSize::PAGE_4K;
    bool             pt_alloc_needed = false;  /* new PT pages required? */
    uint8_t          numa_node = 0;
    uint8_t          core_id   = 0;

    bool operator==(const FaultClass& o) const {
        return mapping == o.mapping && access == o.access &&
               page_size == o.page_size && pt_alloc_needed == o.pt_alloc_needed &&
               numa_node == o.numa_node;
    }
};

/* ------------------------------------------------------------------ */
/*  Lightweight histogram (lock-free, fixed-bucket)                    */
/* ------------------------------------------------------------------ */

/**
 * Fixed-bucket histogram for cycle/ns values.
 * Bucket i covers [base * 2^i, base * 2^(i+1)).
 * Bucket 0 catches everything below base.
 * Last bucket catches overflow.
 */
template <int N_BUCKETS = 24, uint64_t BASE = 10>
class LatencyHistogram {
public:
    LatencyHistogram() { reset(); }

    void record(uint64_t value) {
        int bucket = 0;
        if (value >= BASE) {
            uint64_t shifted = value / BASE;
            /* floor(log2(shifted)) */
            bucket = 63 - __builtin_clzll(shifted);
            if (bucket >= N_BUCKETS) bucket = N_BUCKETS - 1;
        }
        counts_[bucket]++;
        sum_ += value;
        count_++;
        if (value < min_) min_ = value;
        if (value > max_) max_ = value;
    }

    void reset() {
        std::fill(counts_.begin(), counts_.end(), 0);
        sum_ = 0;
        count_ = 0;
        min_ = UINT64_MAX;
        max_ = 0;
    }

    uint64_t count() const { return count_; }
    uint64_t sum()   const { return sum_; }
    uint64_t min_val() const { return min_ == UINT64_MAX ? 0 : min_; }
    uint64_t max_val() const { return max_; }
    double   mean()  const { return count_ > 0 ? (double)sum_ / count_ : 0; }

    uint64_t percentile(double pct) const {
        if (count_ == 0) return 0;
        uint64_t target = (uint64_t)(pct / 100.0 * count_);
        uint64_t cumulative = 0;
        for (int i = 0; i < N_BUCKETS; i++) {
            cumulative += counts_[i];
            if (cumulative >= target) {
                return (i == 0) ? BASE : BASE * (1ULL << i);
            }
        }
        return BASE * (1ULL << (N_BUCKETS - 1));
    }

    const std::array<uint64_t, N_BUCKETS>& buckets() const { return counts_; }

private:
    std::array<uint64_t, N_BUCKETS> counts_;
    uint64_t sum_;
    uint64_t count_;
    uint64_t min_;
    uint64_t max_;
};

/* ------------------------------------------------------------------ */
/*  Per-fault timing record (stack-allocated, one per fault)           */
/* ------------------------------------------------------------------ */

struct FaultPhaseTimings {
    uint64_t phase_start[kNumFaultPhases];
    uint64_t phase_end[kNumFaultPhases];
    uint64_t fault_start;
    uint64_t fault_end;
    FaultClass fault_class;
    uint64_t faulting_va;

    FaultPhaseTimings() {
        memset(phase_start, 0, sizeof(phase_start));
        memset(phase_end, 0, sizeof(phase_end));
        fault_start = fault_end = 0;
        faulting_va = 0;
    }

    void begin_fault(uint64_t now, uint64_t va) {
        fault_start = now;
        faulting_va = va;
    }

    void end_fault(uint64_t now) {
        fault_end = now;
    }

    void begin_phase(FaultPhase p, uint64_t now) {
        phase_start[static_cast<int>(p)] = now;
    }

    void end_phase(FaultPhase p, uint64_t now) {
        phase_end[static_cast<int>(p)] = now;
    }

    uint64_t phase_duration(FaultPhase p) const {
        int i = static_cast<int>(p);
        return (phase_end[i] > phase_start[i]) ? (phase_end[i] - phase_start[i]) : 0;
    }

    uint64_t total_duration() const {
        return (fault_end > fault_start) ? (fault_end - fault_start) : 0;
    }
};

/* ------------------------------------------------------------------ */
/*  RAII phase timer (scoped)                                          */
/* ------------------------------------------------------------------ */

class ScopedPhaseTimer {
public:
    ScopedPhaseTimer(FaultPhaseTimings& t, FaultPhase p)
        : timings_(t), phase_(p)
    {
        timings_.begin_phase(phase_, now());
    }

    ~ScopedPhaseTimer() {
        timings_.end_phase(phase_, now());
    }

private:
    static uint64_t now() {
        /* Use steady_clock for host-side timing.  When running under
           the simulator, this will measure wall-clock cost of the
           MimicOS handler code (not simulated cycles). */
        auto tp = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            tp.time_since_epoch()).count();
    }

    FaultPhaseTimings& timings_;
    FaultPhase phase_;
};

/* ------------------------------------------------------------------ */
/*  Aggregate fault metrics collector                                  */
/* ------------------------------------------------------------------ */

class FaultMetrics {
public:
    FaultMetrics() : enabled_(false), fault_count_(0) {}

    void enable()  { enabled_ = true; }
    void disable() { enabled_ = false; }
    bool is_enabled() const { return enabled_; }

    /**
     * Record a completed fault's timing data into aggregate histograms.
     */
    void record(const FaultPhaseTimings& t) {
        if (!enabled_) return;
        fault_count_++;
        total_hist_.record(t.total_duration());
        for (int i = 0; i < kNumFaultPhases; i++) {
            uint64_t d = t.phase_duration(static_cast<FaultPhase>(i));
            if (d > 0)
                phase_hists_[i].record(d);
        }
    }

    uint64_t fault_count() const { return fault_count_; }

    const LatencyHistogram<>& total_histogram() const { return total_hist_; }
    const LatencyHistogram<>& phase_histogram(FaultPhase p) const {
        return phase_hists_[static_cast<int>(p)];
    }

    /**
     * Dump all metrics to a CSV file consumable by the Python fitter.
     *
     * Format:  metric,phase,stat,value
     */
    void dump_csv(const std::string& filepath) const {
        std::ofstream f(filepath);
        if (!f.is_open()) return;

        f << "metric,phase,stat,value\n";
        f << "total,all,count," << fault_count_ << "\n";
        f << "total,all,mean," << total_hist_.mean() << "\n";
        f << "total,all,min," << total_hist_.min_val() << "\n";
        f << "total,all,max," << total_hist_.max_val() << "\n";
        f << "total,all,p50," << total_hist_.percentile(50) << "\n";
        f << "total,all,p95," << total_hist_.percentile(95) << "\n";
        f << "total,all,p99," << total_hist_.percentile(99) << "\n";

        for (int i = 0; i < kNumFaultPhases; i++) {
            const auto& h = phase_hists_[i];
            if (h.count() == 0) continue;
            const char* pn = fault_phase_name(static_cast<FaultPhase>(i));
            f << "phase," << pn << ",count," << h.count() << "\n";
            f << "phase," << pn << ",mean," << h.mean() << "\n";
            f << "phase," << pn << ",min," << h.min_val() << "\n";
            f << "phase," << pn << ",max," << h.max_val() << "\n";
            f << "phase," << pn << ",p50," << h.percentile(50) << "\n";
            f << "phase," << pn << ",p95," << h.percentile(95) << "\n";
            f << "phase," << pn << ",p99," << h.percentile(99) << "\n";
        }
    }

    /**
     * Dump per-phase bucket histograms for detailed analysis.
     */
    void dump_histograms(const std::string& filepath) const {
        std::ofstream f(filepath);
        if (!f.is_open()) return;

        f << "phase,bucket_idx,bucket_lo,bucket_hi,count\n";
        auto dump_one = [&](const char* name, const LatencyHistogram<>& h) {
            for (int b = 0; b < (int)h.buckets().size(); b++) {
                uint64_t lo = (b == 0) ? 0 : 10 * (1ULL << (b - 1));
                uint64_t hi = 10 * (1ULL << b);
                f << name << "," << b << "," << lo << "," << hi
                  << "," << h.buckets()[b] << "\n";
            }
        };
        dump_one("total", total_hist_);
        for (int i = 0; i < kNumFaultPhases; i++) {
            if (phase_hists_[i].count() > 0)
                dump_one(fault_phase_name(static_cast<FaultPhase>(i)),
                         phase_hists_[i]);
        }
    }

    void reset() {
        fault_count_ = 0;
        total_hist_.reset();
        for (auto& h : phase_hists_) h.reset();
    }

private:
    bool enabled_;
    uint64_t fault_count_;
    LatencyHistogram<> total_hist_;
    LatencyHistogram<> phase_hists_[kNumFaultPhases];
};

/* ------------------------------------------------------------------ */
/*  Calibration parameter set (loaded from config)                     */
/* ------------------------------------------------------------------ */

struct FaultCalibrationParams {
    /* Per-phase cost penalties in nanoseconds.
       When non-zero, these are ADDED to the simulated fault cost to
       account for microarchitectural effects not visible in the
       functional MimicOS path. */
    uint64_t vma_lookup_base_ns        = 0;
    uint64_t pt_page_alloc_cost_ns     = 0;
    uint64_t phys_page_alloc_cost_ns   = 0;
    uint64_t zero_fill_cost_ns         = 0;
    uint64_t pte_install_cost_ns       = 0;
    uint64_t lock_contention_penalty_ns = 0;
    uint64_t cache_tlb_perturb_penalty_ns = 0;
    uint64_t numa_penalty_ns           = 0;

    /* Feature flags */
    bool phase_instrumentation_enabled = false;
    bool per_fault_logging_enabled     = false;

    /* Phase 7 drift-injection controls.  When extra_iters > 0, every
       detailed fault whose running index >= after_faults runs an
       additional LinuxPhase::inject_drift_cycles(extra_iters) at the
       tail of the phase chain.  Purpose: prove the drift detector
       fires on real distribution shifts.  Leave both = 0 for normal
       (non-drift-injection) runs. */
    uint64_t inject_drift_after_faults = 0;
    uint64_t inject_drift_extra_iters  = 0;

    uint64_t total_penalty(const FaultClass& fc) const {
        uint64_t total = vma_lookup_base_ns;
        if (fc.pt_alloc_needed) total += pt_page_alloc_cost_ns;
        total += phys_page_alloc_cost_ns;
        if (fc.mapping == FaultMappingType::ANON) total += zero_fill_cost_ns;
        total += pte_install_cost_ns;
        /* TODO: contention, NUMA based on runtime state */
        return total;
    }
};
