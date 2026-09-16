/**
 * @file fast_fault_profile.h
 * @brief Online train-then-replay profile for anonymous page faults.
 *
 * Phase 1 scaffolding for the fast-fault path.  Each distinct
 * FaultFingerprint gets a FastFaultProfile that moves through:
 *
 *   UNKNOWN -> TRAINING -> TRAINED      (stable; replay fast path)
 *                        -> ABANDONED   (never stabilised; always detailed)
 *
 * During TRAINING the full LinuxPhase helper chain runs and per-fault
 * cycle totals feed a running mean/variance via Welford's algorithm.
 * Once N samples have accumulated with stddev/mean below a threshold,
 * the profile is promoted to TRAINED and the fault handler switches
 * to shipping a SimFastFault magic to Sniper instead of executing the
 * LinuxPhase code locally.
 *
 * Phase 1 stores only summary statistics + a memory_trace template.
 * Phase 2 populates memory_trace with captured (stream_id, offset,
 * stride, op, size) tuples.  Phase 3 drives the cache hierarchy from
 * those tuples at replay time.  This header defines the data shape so
 * later phases can be landed incrementally without moving the struct.
 */
#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <vector>
#include <unordered_map>

#include "mm/fault_signature.h"

/* ------------------------------------------------------------------ */
/*  Profile state machine                                              */
/* ------------------------------------------------------------------ */

enum class FastFaultState : uint8_t {
    UNKNOWN   = 0,  /* never seen — next fault will enter TRAINING */
    TRAINING  = 1,  /* collecting samples, executing detailed path */
    TRAINED   = 2,  /* stable; replay fast path via SimFastFault */
    ABANDONED = 3,  /* too noisy to replay; always detailed */
};

/* ------------------------------------------------------------------ */
/*  Memory-access template (populated in Phase 2)                      */
/* ------------------------------------------------------------------ */

/**
 * One recorded memory access in the LinuxPhase helper chain.
 *
 * At replay time the effective address is computed as
 *   pool_base[stream_id] + (cursor ^ offset_seed) % stream_size * stride
 * so that each replayed fault hits fresh cold cache lines (preserving
 * the dep-chained-read behaviour the calibration depends on) while
 * the simulator's cost for the walk is just the performance-model
 * charge of a handful of cache accesses per tuple.
 *
 * Phase 1 leaves this empty; Phase 2 captures entries via hooks in
 * LinuxPhase::_cold_chain_reads and LinuxPhase::clear_page.
 */
struct MemoryAccessTemplate {
    uint8_t  stream_id;      /* which LinuxPhase pool this read/write hits */
    uint8_t  op;             /* 0=read, 1=write, 2=rmw */
    uint8_t  size_log2;      /* log2(bytes) — 3=8B, 6=64B */
    uint8_t  reserved;
    uint32_t offset_seed;    /* used by replay to derive cold-line offset */
};
static_assert(sizeof(MemoryAccessTemplate) == 8,
              "MemoryAccessTemplate must pack to 8 bytes for magic payload");

/**
 * Per-access replay record captured at training time and re-issued by the
 * fast-replay loop in poll_for_signal.  Because the replay executes real
 * loads/stores against these VAs in DETAILED mode, Sniper's cache model
 * charges hit/miss latency naturally — no need for a synthetic cost anchor
 * or a latency reservoir.
 *
 * op values:
 *   0 = read         — single byte load from vaddr
 *   1 = write        — single byte store to vaddr
 *   2 = rmw          — __atomic_fetch_add on vaddr
 *   3 = zero_sweep   — memset(cold_page_ptr, 0, 1 << size_log2); vaddr
 *                      is unused (caller supplies the cold page)
 *
 * dep_flag = 1 marks accesses whose address depends on the prior access's
 * value (the _cold_chain_reads pattern).  The replay loop XORs the low
 * bits of its rolling `dep` accumulator into the address so Sniper sees
 * the same load-to-load dependency and serialises the misses, matching
 * baseline's cache-hierarchy stall behaviour.  The XOR is masked to stay
 * within the same 64 B cache line so the cold-line target is unchanged.
 */
struct CapturedAccess {
    uint64_t vaddr;          /* target address (meaningful for op 0-2) */
    uint8_t  op;             /* see above */
    uint8_t  size_log2;      /* 3=8B, 6=64B, 12=4 KiB (for zero_sweep) */
    uint8_t  dep_flag;       /* 1 = dep-chain with prior value */
    uint8_t  reserved[5];
};
static_assert(sizeof(CapturedAccess) == 16,
              "CapturedAccess must pack to 16 bytes");

/* ------------------------------------------------------------------ */
/*  Fast-fault profile                                                 */
/* ------------------------------------------------------------------ */

struct FastFaultProfile {
    FastFaultState state = FastFaultState::UNKNOWN;

    /* Running statistics over total_cost cycles (Welford) */
    uint32_t sample_count = 0;
    double   mean_cycles  = 0.0;
    double   m2_cycles    = 0.0;   /* sum of squared deviations */

    /* Captured memory-access template (Phase 2 — legacy summary) */
    std::vector<MemoryAccessTemplate> memory_trace;

    /* Phase 4: real VAs captured during the first training sample.
       Replayed by poll_for_signal's fast_replay_mode branch via real
       loads/stores in DETAILED mode so Sniper charges cache latency
       naturally.  Capped at kMaxCaptured below. */
    std::vector<CapturedAccess> captured_accesses;

    /* Bookkeeping */
    uint32_t fp_id        = 0;     /* stable ID used in SimFastFault magic */
    uint32_t replay_count = 0;     /* times the fast path fired */

    /* Phase 7 drift-detection state.  Populated only when
       policy.resample_every > 0.  Each counter zeroes out on a
       state->TRAINED re-entry (i.e., when the profile is retrained
       after a drift-induced demotion). */
    uint32_t resample_count   = 0; /* detailed re-samples taken in TRAINED */
    uint32_t drift_in_band    = 0; /* resamples inside mean ± kσ */
    uint32_t drift_out_band   = 0; /* resamples outside mean ± kσ (any) */
    uint32_t drift_out_streak = 0; /* current consecutive out-of-band count */
    uint32_t drift_resets     = 0; /* times this profile was demoted to TRAINING */
    double   drift_last_sample = 0.0; /* most recent resample value (diagnostics) */

    /* Phase 8 non-parametric tracking.  Bounded-memory reservoir of
       recent training samples.  Captured in the detailed path on
       every record_sample() call so we can publish empirical
       p50/p90/p99 alongside Welford mean/σ without assuming a
       Gaussian distribution.  Cleared on demotion (same as Welford
       state) so the ring always reflects the current regime.
       Ring-buffer semantics: newest kQuantileCap samples kept; older
       evicted.  256 × 4 B = 1 KiB per profile — negligible. */
    static constexpr size_t kQuantileCap = 256;
    uint32_t quantile_ring[kQuantileCap] = {};
    uint32_t quantile_ring_head = 0;
    uint32_t quantile_ring_size = 0;

    /* Phase 8 / Item C: per-profile pcp-class telemetry.  Populated by
       poll_for_signal from the allocator's last_alloc_fastpath field
       on every training sample.  Surfacing hit/miss counts per profile
       lets us detect bimodal distributions (candidate for future
       hierarchical fingerprint splitting).  Three counters because we
       accept the "unknown" case (allocator doesn't distinguish or pre-
       phase fingerprint). */
    uint32_t pcp_hit_samples     = 0;
    uint32_t pcp_miss_samples    = 0;
    uint32_t pcp_unknown_samples = 0;

    void record_pcp_class(uint8_t cls) {
        if (cls == 0)      pcp_miss_samples++;
        else if (cls == 1) pcp_hit_samples++;
        else               pcp_unknown_samples++;
    }

    /* ---- Running stats helpers ---- */

    void record_sample(uint64_t cycles) {
        sample_count++;
        double x     = static_cast<double>(cycles);
        double delta = x - mean_cycles;
        mean_cycles += delta / sample_count;
        double delta2 = x - mean_cycles;
        m2_cycles   += delta * delta2;

        /* Phase 8: also feed the quantile ring.  uint32 saturating
           cast is fine for cycle counts in [0, 4 e9]. */
        uint32_t clipped = (cycles > UINT32_MAX) ? UINT32_MAX
                                                 : static_cast<uint32_t>(cycles);
        quantile_ring[quantile_ring_head] = clipped;
        quantile_ring_head = (quantile_ring_head + 1) % kQuantileCap;
        if (quantile_ring_size < kQuantileCap) quantile_ring_size++;
    }

    /* Phase 8: empirical quantile from the ring.  O(k log k) in the
       ring size — only call from dump sites, not per-fault hot paths.
       q ∈ [0, 1].  Returns 0 if no samples. */
    uint32_t quantile(double q) const {
        if (quantile_ring_size == 0) return 0;
        std::vector<uint32_t> tmp(quantile_ring,
                                  quantile_ring + quantile_ring_size);
        std::sort(tmp.begin(), tmp.end());
        double idx_d = q * static_cast<double>(tmp.size() - 1);
        size_t idx = static_cast<size_t>(idx_d);
        if (idx >= tmp.size()) idx = tmp.size() - 1;
        return tmp[idx];
    }

    /* Phase 8: reset the quantile ring.  Called on drift-demotion
       alongside Welford reset so the new regime gets fresh data. */
    void clear_quantile_ring() {
        quantile_ring_head = 0;
        quantile_ring_size = 0;
    }

    double stddev() const {
        if (sample_count < 2) return 0.0;
        return std::sqrt(m2_cycles / (sample_count - 1));
    }

    double cv() const {  /* coefficient of variation, sigma/mu */
        if (mean_cycles <= 0.0) return 1.0;
        return stddev() / mean_cycles;
    }
};

/* ------------------------------------------------------------------ */
/*  Registry (fingerprint -> profile)                                  */
/* ------------------------------------------------------------------ */

/**
 * Confidence criterion:
 *   - Require at least kMinSamples before any promotion decision.
 *   - If cv < kStableCV at kMinSamples  -> TRAINED.
 *   - If still cv >= kStableCV at kAbandonSamples  -> ABANDONED.
 *
 * Tunables per the Phase 1 plan: N=100, sigma/mu < 10% gate.  Keep
 * them as constexpr for now — if we end up wanting per-fingerprint
 * policy, move to a struct and thread through configure().
 */
/**
 * Replay-mode selector.  FULL issues every captured access through the
 * cache hierarchy on each replay (highest fidelity, highest cost).
 * PREFIX_K issues only the first K accesses and relies on the barrier
 * clock-advance in SimMimicosResult to charge the remainder — a
 * touch-once / cache-state-checkpoint approximation: warm state is
 * kept coherent via the first K accesses while the bulk cost is paid
 * analytically.  ANALYTICAL skips the SimFastFault magic entirely and
 * charges the full mean_cycles via the barrier advance (zero cache
 * fidelity, maximum wall-time savings).
 */
enum class FastFaultReplayMode : uint8_t {
    FULL       = 0,
    PREFIX_K   = 1,
    ANALYTICAL = 2,
};

struct FastFaultPolicy {
    uint32_t min_samples      = 100;   /* decide TRAINED vs continue */
    uint32_t abandon_samples  = 500;   /* decide ABANDONED */
    double   stable_cv        = 0.10;  /* sigma/mu threshold */
    /* Resample rate in TRAINED state: 1/N faults run detailed to
       re-check that the profile still holds.  0 disables resampling. */
    uint32_t resample_every   = 0;

    /* Replay-strategy knobs (Phase 6 speed techniques). */
    FastFaultReplayMode replay_mode     = FastFaultReplayMode::FULL;
    uint32_t            replay_prefix_k = 16;  /* only used when PREFIX_K */

    /* Dictionary-cache knob (Phase 6 technique B).  When enabled the
       Sniper-side SIM_CMD_FAST_FAULT handler caches the decoded
       CapturedAccess blob keyed by trace_ptr, avoiding N×16 B
       core->accessMemory reads on every replay.  The cache is
       invalidated implicitly when the guest vector's data() pointer
       changes, which only happens during TRAINING capture — after
       promotion the pointer is stable. */
    bool                dict_cache      = true;

    /* Phase 7 drift-detection knobs.  Closed-loop self-calibration.
       When resample_every > 0, every Nth TRAINED fault runs detailed
       instead of fast-replay; the measured sample is compared against
       the profile's mean ± drift_sigma_band × σ band.  After
       drift_consecutive_out consecutive out-of-band samples the
       profile is demoted back to TRAINING, its Welford stats +
       captured_accesses + SimSetFastFaultCharge are reset, and the
       next fault re-captures fresh.  Set resample_every = 0 (default)
       to disable. */
    double   drift_sigma_band     = 3.0;
    uint32_t drift_consecutive_out = 3;

    /* Phase 9: hierarchical fingerprint splitting.  When enabled, the
       MimicOS fault dispatcher partitions the profile space by an
       additional fingerprint dimension (today: pcp_hit_or_miss, as
       reported by PhysicalMemoryAllocator::last_alloc_fastpath).  The
       pre-fault fingerprint uses the *previous* fault's observed pcp
       class as a predictor (initial value: unknown).  This implicitly
       routes to one of two child profiles (pcp=hit vs pcp=miss),
       each with its own Welford stats and TRAINED/ABANDONED state.
       Bimodal workloads that would otherwise saturate at
       stable_cv=0.20 and stay UNKNOWN/ABANDONED now produce two
       tighter profiles, each reachable by replay.

       On phase-coherent workloads the previous-fault predictor is
       >95% accurate (pcp-hit runs cluster into batches of ~32
       between refills on anon_write_calib), so the prediction lag
       is absorbed in the natural cv.  For uncorrelated workloads
       the predictor degrades to random, in which case the parent
       profile's cv is a lower bound on what splitting can achieve
       (future work: predictor training).

       Disabled by default to preserve pre-Phase-9 behaviour. */
    bool hierarchical_split_enabled = false;

    /* Phase 9 revisit: also split along pgtable_install_level
       (none / pmd_install / pud_install).  Independent of
       hierarchical_split_enabled so the dimensions can be enabled
       individually.  When this is true, the dispatcher writes the
       per-fault install-level into FaultFingerprint.pgtable_install_level
       at both pre-fault lookup and post-fault recording sites; lookups
       partition into 3 child profiles.  Predictor is deterministic
       (computed from the fault VA + per-process installed_pmds /
       installed_puds sets) — no statistical predictor lag. */
    bool hierarchical_split_pgtable_enabled = false;

    /* Phase 9-C: split along vma_freshness (fresh / aged).  Detection
       is a per-process distance heuristic: a fault whose VPN is
       further than 512 pages (one PMD chunk) from the prior fault's
       VPN is classified as "fresh" — approximates "this fault came
       right after a mmap that established a new region".  Mutually
       exclusive with pgtable_install_level > 0 in the dispatcher
       (install wins to avoid double-classification).  Independent of
       the other split knobs. */
    bool hierarchical_split_vma_freshness_enabled = false;
};

class FastFaultRegistry {
public:
    FastFaultRegistry() = default;

    void configure(const FastFaultPolicy& p) { policy_ = p; }
    const FastFaultPolicy& policy() const { return policy_; }

    void enable()  { enabled_ = true; }
    void disable() { enabled_ = false; }
    bool is_enabled() const { return enabled_; }

    /**
     * Get-or-insert.  Assigns a monotonic fp_id to newly created
     * profiles so SimFastFault magic payloads can identify the
     * profile without having to hash the full fingerprint every
     * time.
     */
    FastFaultProfile& get_or_insert(const FaultFingerprint& fp) {
        auto it = table_.find(fp);
        if (it != table_.end()) return it->second;
        FastFaultProfile fresh;
        fresh.fp_id = next_fp_id_++;
        auto [ins, _] = table_.emplace(fp, std::move(fresh));
        id_index_[ins->second.fp_id] = &ins->second;
        return ins->second;
    }

    /**
     * Fast lookup by fp_id (used by the Sniper-side handler so it
     * does not have to rebuild the fingerprint).
     */
    FastFaultProfile* find_by_id(uint32_t fp_id) {
        auto it = id_index_.find(fp_id);
        return (it == id_index_.end()) ? nullptr : it->second;
    }

    /**
     * Decide the next state for @p prof given its current samples.
     * Called after record_sample() during TRAINING.
     */
    void maybe_promote(FastFaultProfile& prof) {
        if (prof.state != FastFaultState::TRAINING) return;
        if (prof.sample_count >= policy_.min_samples &&
            prof.cv() < policy_.stable_cv) {
            prof.state = FastFaultState::TRAINED;
            return;
        }
        if (prof.sample_count >= policy_.abandon_samples) {
            prof.state = FastFaultState::ABANDONED;
        }
    }

    /**
     * Phase 7: drift-detection result.  Returned by check_drift() so
     * the caller can log / act.
     */
    enum class DriftAction : uint8_t {
        IN_BAND     = 0,  /* sample within mean ± kσ */
        OUT_BAND    = 1,  /* sample outside band, streak still tolerable */
        DEMOTE      = 2,  /* streak hit drift_consecutive_out → reset profile */
    };

    /**
     * Feed a detailed-mode re-sample from a TRAINED profile into
     * drift tracking.  Must be called only when prof.state ==
     * TRAINED and policy.resample_every > 0.  Does NOT update the
     * profile's Welford stats — those are frozen once TRAINED; the
     * resample is an independent drift check.  If the streak exceeds
     * the consecutive-out threshold, demotes the profile in-place
     * (state → TRAINING, clears Welford + captured_accesses +
     * drift state) and returns DEMOTE.  Caller is responsible for
     * calling SimSetFastFaultCharge(0) on DEMOTE so the Sniper-side
     * barrier advance stops charging the stale mean.
     */
    DriftAction check_drift(FastFaultProfile& prof, uint64_t sample_cyc) {
        prof.resample_count++;
        prof.drift_last_sample = static_cast<double>(sample_cyc);

        double sigma = prof.stddev();
        double lo = prof.mean_cycles - policy_.drift_sigma_band * sigma;
        double hi = prof.mean_cycles + policy_.drift_sigma_band * sigma;
        double x  = prof.drift_last_sample;

        if (x >= lo && x <= hi) {
            prof.drift_in_band++;
            prof.drift_out_streak = 0;
            return DriftAction::IN_BAND;
        }

        prof.drift_out_band++;
        prof.drift_out_streak++;

        if (prof.drift_out_streak >= policy_.drift_consecutive_out) {
            /* Demote: reset Welford + captured trace; keep replay_count +
               drift_resets counters so dumps can show the history.  The
               fp_id stays stable so the Sniper-side dict cache entry for
               the old trace becomes garbage (never reused) but doesn't
               need explicit eviction — its (ptr,len) key will miss when
               the new captured_accesses vector lands at a new data()
               pointer post-shrink_to_fit. */
            prof.state         = FastFaultState::TRAINING;
            prof.sample_count  = 0;
            prof.mean_cycles   = 0.0;
            prof.m2_cycles     = 0.0;
            prof.captured_accesses.clear();
            prof.captured_accesses.shrink_to_fit();
            prof.clear_quantile_ring();  /* Phase 8: fresh ring for new regime */
            prof.drift_out_streak = 0;
            prof.drift_resets++;
            return DriftAction::DEMOTE;
        }
        return DriftAction::OUT_BAND;
    }

    size_t size() const { return table_.size(); }

    /* ---- Diagnostics ---- */
    struct Stats {
        uint64_t trained_promotions   = 0;
        uint64_t abandoned_promotions = 0;
        uint64_t fast_replays         = 0;
        uint64_t detailed_trainings   = 0;
    };
    Stats& stats() { return stats_; }
    const Stats& stats() const { return stats_; }

    const std::unordered_map<FaultFingerprint, FastFaultProfile,
                             FaultFingerprintHash>& table() const {
        return table_;
    }

private:
    bool enabled_ = false;
    FastFaultPolicy policy_;
    Stats stats_;
    uint32_t next_fp_id_ = 1;  /* 0 reserved for "invalid / unknown" */

    std::unordered_map<FaultFingerprint, FastFaultProfile,
                       FaultFingerprintHash> table_;
    std::unordered_map<uint32_t, FastFaultProfile*> id_index_;
};
