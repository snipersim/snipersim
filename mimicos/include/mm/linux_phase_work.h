/**
 * @file linux_phase_work.h
 * @brief Per-phase code that mimics Linux's anonymous-page minor fault.
 *
 * Each function performs a small, structurally-faithful imitation of the
 * corresponding Linux kernel operation.  When MimicOS runs under Sniper+SDE,
 * the cycles Sniper simulates for these instructions become the per-phase
 * fault cost — so the calibration target is "Sniper-simulated cycles per
 * phase ≈ Linux-measured cycles per phase from eBPF/bench".
 *
 * Linux references (v6.6):
 *   arch/x86/mm/fault.c:1239   do_user_addr_fault
 *   mm/memory.c:5031           __handle_mm_fault
 *   mm/memory.c:4067           do_anonymous_page
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <atomic>
#include <vector>

#include "mm/fast_fault_profile.h"

/* Apr 19 2026: turn OFF optimisation for the entire LinuxPhase namespace.
 * When MimicOS itself is built at -O2 (for speed), the compiler inlines
 * these calibration functions into the fault loop and then aggressively
 * elides the cache-cold reads / atomic RMWs / store loops that produce
 * our measured Sniper cycle cost — per-phase means collapse by 3-14×
 * (e.g. trap_entry 442→31, vma_lookup 878→471, total 5391→1786 cyc),
 * invalidating the calibration.  Forcing these functions to -O0 keeps
 * the instruction stream the compiler emits faithful to the source, so
 * Sniper simulates the same work regardless of outer optimisation.
 * The cost is localised: only the phase-work hot path pays -O0, while
 * poll_for_signal and the rest of mimicos.cc still benefit from -O2.
 * Use the GCC pragma form (supported by gcc 4.4+ and clang with some
 * limitations).  If this ever needs to work under clang, we'll fall back
 * to __attribute__((optnone)) per-function.
 */
#pragma GCC push_options
#pragma GCC optimize ("O0")

namespace LinuxPhase {

/* ---------- Shared primitives (must be declared before first use) --------
 * `PaddedAtomic` and `_cold_chain_reads` are used across multiple phase
 * helpers below.  Keep their definitions early in the namespace so any
 * phase struct (pcp, kernel-entry, pte-install, …) can use them without
 * forward-reference issues. */
struct alignas(64) PaddedAtomic {
    std::atomic<uint64_t> val;
    uint8_t pad[64 - sizeof(std::atomic<uint64_t>)];
};

/* Dependency-chained cold read: each successive load's *address* depends
   on the prior load's *result*, which forces Sniper to serialise the
   loads and actually stall on cache-cold lines instead of pipelining
   them invisibly.  Returns the final chained value — caller must store
   into `volatile` to prevent DCE. */
/* Forward decls for the capture machinery — definitions below the
   summary-capture block.  Two flavours: the non-capturing variant is
   what runs on every fault (including all steady-state replays of
   already-TRAINED profiles), and it must have ZERO capture overhead.
   The _capturing variant is dispatched only on the first training
   sample per fingerprint via an outer `if (g_capture_target)` branch,
   so its extra call+push_back cost is paid once per profile instead
   of once per fault. */
static inline void capture_access_vaddr(uint64_t vaddr, uint8_t op,
                                        uint8_t size_log2, uint8_t dep_flag);

/* Hot path: no instrumentation, pristine dep chain. */
static inline uint64_t _cold_chain_reads(const uint8_t* pool, size_t lines,
                                         uint64_t seed, int n)
{
    uint64_t dep = seed;
    for (int i = 0; i < n; i++) {
        uint64_t off = ((dep * 2654435761u + i * 0x9E3779B97F4A7C15ULL) % lines) * 64;
        const uint8_t* p = pool + off;
        dep ^= *reinterpret_cast<const uint64_t*>(p);
    }
    return dep;
}

/* Slow path: identical body but records the real VA per iteration.
   Only called when the per-fault dispatcher has confirmed
   g_capture_target != nullptr.  The compiler can't elide the extra
   call under -O0, so the caller MUST dispatch to the non-capturing
   variant when capture is off. */
static inline uint64_t _cold_chain_reads_capturing(
    const uint8_t* pool, size_t lines, uint64_t seed, int n)
{
    uint64_t dep = seed;
    for (int i = 0; i < n; i++) {
        uint64_t off = ((dep * 2654435761u + i * 0x9E3779B97F4A7C15ULL) % lines) * 64;
        const uint8_t* p = pool + off;
        capture_access_vaddr(reinterpret_cast<uint64_t>(p),
                             /*op=read*/0, /*64B*/6, /*dep_flag=*/1);
        dep ^= *reinterpret_cast<const uint64_t*>(p);
    }
    return dep;
}

/* ---------- Phase 7 drift-injection primitive ---------------------------
 * Deterministic, configurable-size cost injection for induced-drift
 * validation.  Drives N dep-chained cold-line reads against a dedicated
 * 64 KiB pool (independent of the phase helpers' pools, so injection
 * noise doesn't perturb the legitimate phase-cost measurements).  Cost
 * scales linearly at ~237 sim cyc per iteration (same as the other
 * `_cold_chain_reads` call sites).
 *
 * Used only by the fault driver in mimicos.cc when
 * FaultCalibrationParams::inject_drift_extra_iters > 0 AND the running
 * fault index has exceeded inject_drift_after_faults.  Never called on
 * fast-replay faults — only on detailed ones (where the profile is
 * watching for drift via the resample cadence). */
static inline uint64_t inject_drift_cycles(int n) {
    static constexpr size_t kPoolBytes = 64 * 1024;   /* 64 KiB */
    alignas(64) static uint8_t s_drift_pool[kPoolBytes] = {};
    constexpr size_t kLines = kPoolBytes / 64;
    /* Seed with a fixed non-zero value — replay-determinism not needed
       here; what matters is that per-iteration cost is predictable. */
    return _cold_chain_reads(s_drift_pool, kLines, 0xDEADBEEFCAFEBABEULL, n);
}

/* ---------- Phase 9-B page-table install cost model -----------------------
 * Empirical real-Linux fault_class_bench (kratos20, kernel 5.15) shows
 * that anon-write faults at PMD/PUD chunk boundaries cost more than
 * steady faults because the kernel must allocate and zero a fresh
 * page-table page (one for level 1 / two for level 2):
 *   level 0 (steady)      p50 1.39 µs     baseline
 *   level 1 (PMD install) p50 2.54 µs     +1.15 µs ≈ +3300 cyc @ 2.9 GHz
 *   level 2 (PUD install) p50 3.72 µs     +2.33 µs ≈ +6700 cyc total
 * MimicOS's normal phase chain has no separate accounting for this
 * cost — the LinuxPhase helpers run identically regardless of whether
 * a PMD/PUD allocation actually happened.  This helper fires from
 * `poll_for_signal` inside the PHYS_ALLOC bracket whenever the fault
 * fingerprint's `pgtable_install_level > 0`, charging the level-
 * specific Δ over steady.
 *
 * Why two pools (pmd_install_pool, pud_install_pool):
 *   The kernel allocates PMD and PUD page tables from independent
 *   slabs / pcp pools, so their cache lines don't share state.
 *   Using a single pool would let the PUD-install case re-hit cache
 *   lines warmed by a recent PMD-install — collapsing the two-level
 *   cost difference back into one.
 *
 * Calibration (kIters_pmd = 14, kIters_pud_extra = 14):
 *   Each `_cold_chain_reads` iteration lands ~237 cyc on kratos20
 *   (the same anchor as PcpAllocState::kIters).  14 iters → +3300
 *   cyc, matching the empirical level-1 Δ; level 2 runs both the
 *   PMD chain AND a second pud-pool chain → +6700 cyc total.
 *   Tunable via PgtableInstallState::iters_pmd_override /
 *   iters_pud_extra_override (-1 ⇒ use default).  No capture hooks
 *   here: the install dimension is deterministic-from-VA, not
 *   workload-dependent, so trace-capture for replay isn't needed —
 *   the analytical replay path's barrier-advance handles it via
 *   the per-profile mean. */
struct PgtableInstallState {
    static constexpr size_t POOL_BYTES = 64 * 1024;   /* 64 KiB each */
    static constexpr size_t LINES      = POOL_BYTES / 64;
    /* Tuned 2026-05-08 on kratos20 against the fault_class_bench
       deltas (PMD +3335 cyc, PUD +6757 cyc total).  At kIters_pmd=14
       the helper overshot PMD by 42 % (effective per-iter cost ~412
       cyc, higher than the 237 cyc anchor because the per-process
       install pools start cold).  Halving to 8 lands the PMD Δ at
       ~3300 cyc; PUD-extra 13 lands the cumulative Δ at ~6700 cyc. */
    static constexpr int    kIters_pmd        = 8;
    static constexpr int    kIters_pud_extra  = 13;

    alignas(64) uint8_t pmd_install_pool[POOL_BYTES];
    alignas(64) uint8_t pud_install_pool[POOL_BYTES];
    uint64_t cursor = 0;
    int iters_pmd_override       = -1;
    int iters_pud_extra_override = -1;
};

/* level: 0 = none (no-op), 1 = PMD install, 2 = PUD install (also
   includes a PMD chain because every PUD install logically requires a
   subordinate PMD install). */
static inline void install_pgtable_cost(PgtableInstallState& s,
                                        uint64_t va, uint8_t level) {
    if (level == 0) return;
    uint64_t seed = (s.cursor++) ^ va;
    int n_pmd = (s.iters_pmd_override >= 0)
        ? s.iters_pmd_override : PgtableInstallState::kIters_pmd;
    int n_pud_extra = (s.iters_pud_extra_override >= 0)
        ? s.iters_pud_extra_override : PgtableInstallState::kIters_pud_extra;

    static volatile uint64_t sink;
    /* Always pay the PMD-install chain at level >= 1. */
    sink = _cold_chain_reads(s.pmd_install_pool,
                             PgtableInstallState::LINES, seed, n_pmd);
    /* Level 2 adds the PUD-install chain on top, against an
       independent pool so its cache lines don't alias the PMD chain. */
    if (level >= 2) {
        sink = _cold_chain_reads(s.pud_install_pool,
                                 PgtableInstallState::LINES,
                                 seed ^ 0x9E3779B97F4A7C15ULL,
                                 n_pud_extra);
    }
    (void)sink;
}

/* ---------- Phase 9-E PCP buddy-refill cost model -------------------------
 * Empirical real-Linux per-fault eBPF measurement (kprobe:rmqueue_bulk +
 * uprobe wrappers, see experiments/calibration/fault_bench/pcp_class.bt)
 * on kratos20 anon_write, n=10 000:
 *   PCP hit  (98.4%)  p50 4 092 cyc
 *   PCP miss ( 1.6%)  p50 11 642 cyc        Δ = +7 550 cyc (+185%)
 *
 * The Phase-9 fingerprint already partitions the registry along
 * pcp_hit_or_miss, but until this helper landed the simulator's hit
 * and miss profiles converged to nearly identical means (~5 500 cyc
 * each) because pcp_alloc_cost ran identically regardless of the
 * allocator's actual fast-path outcome.  This helper fires AFTER
 * locked_allocate returns, gated on
 * `physical_memory_allocator->last_alloc_fastpath == 0` (= miss),
 * charging the buddy-refill Δ on top of the regular pcp_alloc_cost.
 *
 * Mechanism imitated:
 *   __rmqueue_pcplist returns NULL → __rmqueue_smallest walks the
 *   buddy free-list, splits a higher-order page if needed, and
 *   refills the per-CPU pageset with a batch (~32-128 pages).  The
 *   cost is dominated by the dep-chained free-list walk and a
 *   zone_lock acquire/release pair.
 *
 * Calibration (kIters = 11, tuned 2026-05-08 vs first miss profile
 * with 4 samples):
 *   At kIters=18 the helper overshot the target by +55 % (mean miss
 *   17 133 cyc vs hit 5 412 cyc → Δ = +11 721 cyc, target Δ = +7 550
 *   cyc).  Reason: misses are rare (~1 % rate on this workload), so
 *   the buddy_refill_pool is *always* cold when the helper fires —
 *   per-iter cost lands at ~660 cyc (cold-line latency), not the
 *   ~412 cyc anchor seen on warmer pools.  kIters = 11 × 660 ≈
 *   7 260 cyc, matching Linux Δ within 4 %.  Tunable via
 *   iters_override (-1 ⇒ default). */
struct BuddyRefillState {
    static constexpr size_t POOL_BYTES = 64 * 1024;   /* 64 KiB */
    static constexpr size_t LINES      = POOL_BYTES / 64;
    static constexpr int    kIters     = 11;

    alignas(64) uint8_t buddy_refill_pool[POOL_BYTES];
    alignas(64) PaddedAtomic zone_lock[1024];
    uint64_t cursor = 0;
    int iters_override = -1;
};

static inline void buddy_refill_cost(BuddyRefillState& s, uint64_t va) {
    uint64_t seed = (s.cursor++) ^ va;
    int n = (s.iters_override >= 0) ? s.iters_override : BuddyRefillState::kIters;

    static volatile uint64_t sink;
    /* Dep-chained cold reads model __rmqueue_smallest's free-list walk
       + the buddy-page-split chain.  Independent pool from
       PcpAllocState::freelist_pool so the refill chain doesn't alias
       lines warmed by the preceding pcp_alloc_cost call. */
    sink = _cold_chain_reads(s.buddy_refill_pool,
                             BuddyRefillState::LINES, seed, n);
    (void)sink;

    /* One atomic RMW models the zone_lock acquire/release pair around
       the buddy refill (already-uncontended on a single-thread bench,
       so cost is just the locked-cmpxchg latency). */
    s.zone_lock[seed & 1023].val.fetch_add(1, std::memory_order_release);
}

/* ---------- Phase-2 trace capture hooks ----------------------------------
 * When set non-null by the fault-path driver, each LinuxPhase helper
 * appends one or more MemoryAccessTemplate entries describing what it
 * just did.  Thread-local so multi-core training of different profiles
 * stays isolated.  Capped at kMaxCaptureEntries per profile to keep
 * per-fault overhead bounded (~32 entries covers the whole anon_write
 * phase chain).  The "what to record" granularity matches what Phase 3
 * will need to drive the cache hierarchy (stream_id + op + size +
 * cold-line-offset seed) — not a full instruction trace.
 *
 * Capture is intended to fire only on the FIRST training sample per
 * fingerprint (memory_trace is then frozen for the life of the
 * profile), so the per-fault overhead is one branch-and-check per
 * helper invocation in steady state. */
static constexpr size_t kMaxCaptureEntries = 32;

inline thread_local FastFaultProfile* g_capture_target = nullptr;

/* Stream identifiers — one per LinuxPhase pool.  Phase 3's replay
   handler maps these back to pool base addresses so cold-line access
   patterns are reproduced. */
enum CaptureStream : uint8_t {
    STREAM_PCP_FREELIST  = 0,
    STREAM_KENTRY_IDT    = 1,
    STREAM_KENTRY_PCPU   = 2,
    STREAM_PTE_SCRATCH   = 3,
    STREAM_ZERO_POOL     = 4,
    STREAM_BK_COUNTERS   = 5,
    STREAM_TRAP_SCRATCH  = 6,
    STREAM_TLB_SCRATCH   = 7,
    STREAM_VMA_NODES     = 8,
};

/* Caller guarantees g_capture_target != nullptr (same contract as
   capture_access_vaddr).  Phase-2 legacy summary trace kept for
   backward-compat dumps; Phase 4 replay uses captured_accesses. */
static inline void capture_access(uint8_t stream_id, uint8_t op,
                                  uint8_t size_log2, uint32_t offset_seed)
{
    FastFaultProfile* t = g_capture_target;
    if (t->memory_trace.size() >= kMaxCaptureEntries) return;
    MemoryAccessTemplate e;
    e.stream_id   = stream_id;
    e.op          = op;
    e.size_log2   = size_log2;
    e.reserved    = 0;
    e.offset_seed = offset_seed;
    t->memory_trace.push_back(e);
}

/* Phase 4: per-memory-access capture with real VA.  Called from inside
   each LinuxPhase helper at the exact load/store site, so the recorded
   vaddr is the actual line Sniper will simulate.  dep_flag=1 marks loads
   whose address depended on the prior load's result (_cold_chain_reads
   pattern); the replay loop preserves that dependency by XOR-ing the
   low bits of its rolling `dep` accumulator into the vaddr (within the
   64 B cache line, so the target line is unchanged but Sniper's
   out-of-order model serialises the misses the same way baseline did). */
static constexpr size_t kMaxCaptured = 256;

/* Caller guarantees g_capture_target != nullptr — this is the fast
   push-back; no null check.  The guarantee comes from either:
     - _cold_chain_reads_capturing (called under an outer dispatch)
     - explicit `if (LinuxPhase::g_capture_target) { capture_access_vaddr(...) }`
       wrapper at each atomic / store site. */
static inline void capture_access_vaddr(uint64_t vaddr, uint8_t op,
                                        uint8_t size_log2, uint8_t dep_flag)
{
    FastFaultProfile* t = g_capture_target;
    if (t->captured_accesses.size() >= kMaxCaptured) return;
    CapturedAccess e{};
    e.vaddr     = vaddr;
    e.op        = op;
    e.size_log2 = size_log2;
    e.dep_flag  = dep_flag;
    t->captured_accesses.push_back(e);
}

/* ---------- VMA-lookup imitation -----------------------------------------
 * Linux: lock_vma_under_rcu walks the maple tree of VMAs.  For a typical
 * application with N≈30-200 VMAs the cost is dominated by 2-4 cache-cold
 * line fetches plus a few branchy comparisons (~50-200 cycles).
 *
 * We model it as a binary search over a small (8-element) sorted VPN
 * range table.  The table is populated lazily from observed faulting VAs
 * so the cost scales with the working set, just like a real maple tree.
 */
struct VmaCacheEntry {
    uint64_t vpn_start;
    uint64_t vpn_end;
};

class VmaCache {
public:
    VmaCache() : count_(0) {}

    /* Look up a VPN; returns true if found, false if missing.  In either
       case the binary-search work happens (the cost we care about). */
    bool lookup(uint64_t vpn) {
        int lo = 0, hi = count_ - 1;
        bool hit = false;
        while (lo <= hi) {
            int mid = (lo + hi) >> 1;
            uint64_t s = entries_[mid].vpn_start;
            uint64_t e = entries_[mid].vpn_end;
            if (vpn < s) hi = mid - 1;
            else if (vpn > e) lo = mid + 1;
            else { hit = true; break; }
        }
        return hit;
    }

    /* Cheap insert: round VPN to a 256-page region and insert sorted.
       Limited to MAX_ENTRIES; replaces the first entry if full
       (mimics LRU-like maple-tree growth). */
    void note(uint64_t vpn) {
        uint64_t s = vpn & ~uint64_t(255);
        uint64_t e = s + 255;
        int idx = 0;
        while (idx < count_ && entries_[idx].vpn_end < s) idx++;
        if (idx < count_ && entries_[idx].vpn_start == s) return;  /* dup */
        if (count_ < MAX_ENTRIES) {
            for (int i = count_; i > idx; i--) entries_[i] = entries_[i - 1];
            entries_[idx] = {s, e};
            count_++;
        }
    }

private:
    /* 4 entries × 16 B = 64 B, alignas(64) forces single-cache-line
       placement so the binary search's log2(4)=2 compares hit the
       SAME line instead of 4 different cold lines as with the 16-
       entry version.  Prior experiment with this shrink without the
       option-C hook fix (commit 579be629) showed unstable
       side-effects on neighbouring phases due to capture-hook noise;
       with that noise gone, the shrink should reliably cut vma_lookup
       from ~1430 cyc to ~300 cyc.
       Working-set implications: anon_write_calib touches 10 000 pages
       across 40 256-page regions, so 4 entries is far smaller than
       the working set.  note() just rotates entries; lookup() returns
       an imprecise hit/miss result.  Acceptable because we care about
       the cost model, not the structural accuracy of the VMA tree
       (Linux's maple tree also has bounded warm-path depth). */
    static constexpr int MAX_ENTRIES = 4;
    alignas(64) VmaCacheEntry entries_[MAX_ENTRIES];
    int count_;
};

/* ---------- clear_page imitation ------------------------------------------
 * Linux clear_page() zeros a 4-KB page using MOVNTI / REP STOSQ.  At ~5 GB/s
 * on modern x86 this is ~600 cycles for a 4-KB page when the target frame
 * is cache-cold (which it is — it just came off the buddy / pcp list and
 * has been sitting unreferenced since the last free).
 *
 * Under Sniper, we approximate the cost with 64 stores (one per 64-byte
 * line) to the provided buffer.  CRUCIAL: the caller must rotate through
 * a pool of distinct 4-KB buffers large enough to exceed the simulated
 * LLC, so each fault's stores hit cold cache lines.  If the caller reuses
 * the same 4-KB scratch buffer, the writes stay L1-warm and zero_fill
 * collapses to a deterministic ~320 cyc — well below Linux's measured
 * ~600+ cyc warm-path figure and with none of the DRAM / LLC pressure a
 * real first-touch would produce.  See ColdPagePool below for the helper
 * that manages the rotation.
 */
static inline void clear_page(uint8_t* page_buf) {
    if (g_capture_target) {
        capture_access(STREAM_ZERO_POOL, /*op=write*/1, /*size_log2=12*/12,
                       /*offset_seed=*/0);
        capture_access_vaddr(reinterpret_cast<uint64_t>(page_buf),
                             /*op=zero_sweep*/3, /*size_log2=4KiB*/12,
                             /*dep_flag=*/0);
    }
    /* 128 8-byte stores (2 per cache line × 64 lines) on a cache-cold
       4 KiB frame from ColdPagePool.  Targets the measured Linux
       `clear_page` cost (~600 cyc warm-path on kratos20 per fault_bench).
       History: v2 did 64 stores on a warm scratch buffer (~320 cyc, too
       cheap).  v4 bumped to 512 stores to match REP STOSQ structurally,
       but Sniper models each store at ~7 cyc/op so the total ran to
       ~3 591 cyc — ~6× over real Linux.  Sniper's per-store latency is
       higher than real x86's pipelined MOVNTIQ (~1.2 cyc/store), so
       fewer stores produce the observed cost that matches measurement.
       Structural fidelity: 128 stores still touch every cache line of
       the page (2 stores per line), so the cold-pool eviction and
       LLC/DRAM pressure are captured; we're only dropping the
       intra-line store-buffer pressure that Sniper over-charges. */
    uint64_t* p = reinterpret_cast<uint64_t*>(page_buf);
    for (int i = 0; i < 128; i++) {
        /* Stride of 4 qwords (= 32 bytes) so 128 × 32 = 4 096 covers the
           full page, 2 stores per 64-byte line. */
        reinterpret_cast<volatile uint64_t*>(p)[i * 4] = 0;
    }
}

/* ---------- Cold-frame pool for clear_page --------------------------------
 * Rotating pool of 4-KB buffers.  Callers take one buffer per fault via
 * next_page(); successive calls return non-overlapping 4-KB segments in a
 * ring, so after pool_bytes bytes have been handed out the pool wraps and
 * the earliest buffer is returned again — by then it has been evicted
 * from the simulated LLC (pool is sized > LLC).  Heap-allocated so it
 * doesn't bloat the .bss of startup_mimicos.
 */
struct ColdPagePool {
    uint8_t* pool       = nullptr;
    size_t   pool_bytes = 0;
    size_t   cursor     = 0;

    void init(size_t bytes) {
        pool_bytes = bytes & ~size_t(4095);   /* page-align */
        pool = new uint8_t[pool_bytes];
        /* deliberately do NOT pre-fault / pre-zero — let the first pass
           through the pool fault-in real cold lines from DRAM */
    }
    ~ColdPagePool() { delete[] pool; }

    uint8_t* next_page() {
        if (!pool) return nullptr;
        size_t off = cursor;
        cursor += 4096;
        if (cursor >= pool_bytes) cursor = 0;
        return pool + off;
    }
};

/* ---------- pcp / zone-lock allocator-cost imitation -----------------------
 * Apr 20 2026: when the rest of MimicOS is built at -O2, the GCC template
 * optimiser strips most of the Linux-mimic cost out of
 * `LinuxBuddyAnonAllocator::allocate` + `Buddy::allocate_batch_order0`
 * (GCC pragma/attribute don't propagate through template instantiation).
 * phys_alloc mean collapses 2 227 → 235 cyc and the total fault cost
 * regresses from +8 % vs Linux to −37 %.
 *
 * Rather than fight the optimiser, we add a compensating cost model here
 * — a LinuxPhase::* helper invoked INSIDE the PHYS_ALLOC phase bracket
 * before the real allocator call.  Because this function lives in the
 * `LinuxPhase::` namespace it inherits the file-scope
 * `#pragma GCC optimize ("O0")`, so the dep-chained cold reads and
 * atomic RMWs actually survive into the simulated instruction stream.
 *
 * Structurally, this imitates Linux's `__rmqueue_pcplist` warm path:
 *  - Per-CPU pageset lookup: cache-cold load of `pcp_pageset[cpu]`
 *  - `zone_lock` fast-path: uncontended atomic cmpxchg on a cold line
 *  - `__rmqueue_smallest` free-list walk: several cache-cold list-head loads
 *  - `expand()` split chain when order-0 empty: a few more cold loads
 *
 * Iteration count (default 30 dep-chained reads + 2 atomic RMWs) was
 * tuned on kratos20 to reproduce the v6-anchor phys_alloc mean cost
 * (~2 000 cyc) under MimicOS -O2 builds, within ±10 % of the filtered
 * Linux mean.  Change via PcpAllocState::kIters.
 */
struct PcpAllocState {
    static constexpr size_t ARRAY_BYTES = 4 * 1024 * 1024;   /* 4 MiB */
    static constexpr size_t LINES       = ARRAY_BYTES / 64;  /* 65 536 */
    /* Default iteration count for the dep-chained cold read loop.
       Each iter costs ~237-400 simulated cyc on kratos20 depending on
       where the line lands (LLC vs DRAM) and how much DRAM queueing
       contention is happening.
       History:
         kIters=8 (pre-2026-04-21): phys_alloc ~2 610 cyc with
           MAX_ENTRIES=16 VmaCache; 2 054 cyc after option-C hook fix.
         kIters=5 (current): with MAX_ENTRIES=4 VmaCache, phys_alloc
           drops to ~2 400 cyc, total fault ~5 400 cyc (+8 % vs Linux
           filtered 4 993 — within the under-10 % target).
       Tunable at runtime via `iters_override`. */
    static constexpr int kIters = 5;

    alignas(64) uint8_t      freelist_pool[ARRAY_BYTES];  /* cold-read target  */
    alignas(64) PaddedAtomic zone_lock[1024];             /* 64 KiB — L1-class */
    uint64_t                 cursor = 0;
    int                      iters_override = -1;         /* -1 ⇒ use kIters */
};

static inline void pcp_alloc_cost(PcpAllocState& s, uint64_t va) {
    uint64_t seed = s.cursor++ ^ va;
    int n = (s.iters_override >= 0) ? s.iters_override : PcpAllocState::kIters;

    /* Dep-chained cold reads model the pcp/free-list walk.  Dispatch
       to capturing vs non-capturing variant ONCE per call so the hot
       path (g_capture_target == nullptr, which is every fault except
       the first training sample of each profile) pays zero extra
       cost.  Without this split the per-iteration null check added
       ~5-8 insts/iter × 8 iter, enough to perturb Sniper's ROB model
       and produce phase-mean variance of ±5-20× across runs. */
    static volatile uint64_t sink;
    if (g_capture_target) {
        sink = _cold_chain_reads_capturing(s.freelist_pool,
                                           PcpAllocState::LINES, seed, n);
    } else {
        sink = _cold_chain_reads(s.freelist_pool,
                                 PcpAllocState::LINES, seed, n);
    }
    if (g_capture_target) {
        capture_access(STREAM_PCP_FREELIST, /*op=read*/0, /*size_log2=6*/6,
                       static_cast<uint32_t>(seed));
    }

    /* Two atomic RMWs model the zone_lock acquire + release. */
    if (g_capture_target) {
        capture_access_vaddr(
            reinterpret_cast<uint64_t>(&s.zone_lock[(seed * 1) & 1023].val),
            /*op=rmw*/2, /*8B*/3, /*dep_flag=*/0);
    }
    s.zone_lock[(seed * 1 ) & 1023].val.fetch_add(1, std::memory_order_release);
    if (g_capture_target) {
        capture_access_vaddr(
            reinterpret_cast<uint64_t>(&s.zone_lock[(seed * 17) & 1023].val),
            /*op=rmw*/2, /*8B*/3, /*dep_flag=*/0);
    }
    s.zone_lock[(seed * 17) & 1023].val.fetch_add(1, std::memory_order_release);
    if (g_capture_target) {
        capture_access(STREAM_PCP_FREELIST, /*op=rmw*/2, /*size_log2=3*/3,
                       static_cast<uint32_t>(seed * 17));
    }
}

/* ---------- PTE install imitation -----------------------------------------
 * Linux set_pte_at + folio_add_new_anon_rmap + folio_add_lru_vma + atomic
 * counter increments.  ~50-200 cycles total when the relevant cache lines
 * are warm (the common case during a workload's steady state).
 *
 * Originally we padded these to separate cache lines, which produced
 * realistic cold-line costs but overshot Linux warm-path numbers under
 * Sniper.  Pack into a single cache line for the warm-path approximation.
 */
/* _cold_chain_reads moved to shared-primitives block near top of namespace. */

struct PteInstallState {
    std::atomic<uint64_t> pte_slot;       /* the PTE we "wrote"             */
    std::atomic<uint64_t> mm_counter;     /* like inc_mm_counter            */
    uint64_t              lru_head;       /* like folio_add_lru_vma         */
    /* Additional state to model Linux subsystems Linux does inside
       do_anonymous_page that we previously skipped: */
    std::atomic<uint64_t> ptl_lock;       /* pte_offset_map_lock spinlock   */
    std::atomic<uint64_t> anon_vma_refcnt;/* anon_vma_prepare counter       */
    std::atomic<uint64_t> cgroup_charge;  /* mem_cgroup_charge atomic       */

    /* 4 MiB cold-line scratch: models the freshly-allocated PT page
       that `set_pte_at` writes into (always cache-cold the first time),
       plus the `folio` / `mem_cgroup` accounting lines which are LLC-
       to-DRAM in steady state for a workload that isn't fault-storming. */
    static constexpr size_t SCRATCH_BYTES = 4 * 1024 * 1024;
    static constexpr size_t SCRATCH_LINES = SCRATCH_BYTES / 64;
    alignas(64) uint8_t   scratch[SCRATCH_BYTES];
    uint64_t              cursor = 0;
};

static inline void install_pte(PteInstallState& s, uint64_t pa, uint64_t vpn) {
    /* Capture all three lock/rmw sites under one outer guard. */
    if (g_capture_target) {
        capture_access_vaddr(reinterpret_cast<uint64_t>(&s.anon_vma_refcnt),
                             /*op=rmw*/2, /*8B*/3, /*dep=*/0);
        capture_access_vaddr(reinterpret_cast<uint64_t>(&s.cgroup_charge),
                             /*op=rmw*/2, /*8B*/3, /*dep=*/0);
        capture_access_vaddr(reinterpret_cast<uint64_t>(&s.ptl_lock),
                             /*op=rmw*/2, /*8B*/3, /*dep=*/0);
    }
    /* anon_vma_prepare: increment refcount on shared anon_vma. */
    s.anon_vma_refcnt.fetch_add(1, std::memory_order_relaxed);
    /* mem_cgroup_charge. */
    s.cgroup_charge.fetch_add(4096, std::memory_order_relaxed);
    /* pte_offset_map_lock: take page-table spinlock (cmpxchg loop). */
    uint64_t expected = 0;
    while (!s.ptl_lock.compare_exchange_weak(expected, 1,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
        expected = 0;  /* unlocked when we expect 0 */
        break;          /* uncontended; one cmpxchg models the lock cost */
    }

    /* Linux's real set_pte_at + folio_add_new_anon_rmap + folio_add_lru_vma
       reads + writes several cache-cold lines: the freshly-allocated PT
       slot itself, the folio metadata (page descriptor), per-mm accounting,
       the LRU list head.  Model 4 dep-chained cold loads against the
       4 MiB scratch so Sniper stalls on each L2/LLC-miss line.  ~200 cyc
       of real work that our prior single-cache-line PteInstallState
       missed. */
    static volatile uint64_t sink;
    uint64_t seed = s.cursor++ ^ vpn;
    if (g_capture_target) {
        sink = _cold_chain_reads_capturing(s.scratch,
                                           PteInstallState::SCRATCH_LINES,
                                           seed, 4);
        capture_access(STREAM_PTE_SCRATCH, /*op=read*/0, /*size_log2=6*/6,
                       static_cast<uint32_t>(seed));
        capture_access_vaddr(reinterpret_cast<uint64_t>(&s.pte_slot),
                             /*op=write*/1, /*8B*/3, /*dep=*/0);
        capture_access_vaddr(reinterpret_cast<uint64_t>(&s.mm_counter),
                             /*op=rmw*/2, /*8B*/3, /*dep=*/0);
        capture_access_vaddr(reinterpret_cast<uint64_t>(&s.lru_head),
                             /*op=write*/1, /*8B*/3, /*dep=*/0);
        capture_access_vaddr(reinterpret_cast<uint64_t>(&s.ptl_lock),
                             /*op=write*/1, /*8B*/3, /*dep=*/0);
    } else {
        sink = _cold_chain_reads(s.scratch, PteInstallState::SCRATCH_LINES,
                                 seed, 4);
    }

    /* set_pte_at: atomic store to the PTE slot */
    uint64_t pte = (pa & ~uint64_t(0xFFF)) | 0x67;  /* P|RW|US|A|D */
    s.pte_slot.store(pte, std::memory_order_release);

    /* inc_mm_counter (MM_ANONPAGES) */
    s.mm_counter.fetch_add(1, std::memory_order_relaxed);

    /* folio_add_lru_vma */
    s.lru_head = vpn;

    /* pte_unmap_unlock */
    s.ptl_lock.store(0, std::memory_order_release);
    if (g_capture_target) {
        capture_access(STREAM_PTE_SCRATCH, /*op=rmw*/2, /*size_log2=3*/3,
                       static_cast<uint32_t>(vpn));
    }
}

/* ---------- Trap entry imitation ------------------------------------------
 * Linux do_user_addr_fault prologue: error code parse, perf_sw_event,
 * irq enable, kprobe check, vsyscall check.  ~30-100 cycles total.
 * Modelled as a few cache-line-resident counter updates plus a branchy
 * decision tree.
 *
 * On top of that, every page fault on x86 also pays the HW page-fault
 * vector sequence: IDT entry fetch, stack switch to kernel IST, CR2 read,
 * SWAPGS, then the asm stub to `do_page_fault`.  Measured at ~200-400
 * cyc warm (dominated by stack switch + IDT line read which is cold if
 * faults are infrequent).  fault_bench's rdtscp bracket includes ALL of
 * this; our simulator's phase CSV doesn't — which is the single biggest
 * reason Sniper's p50 is 7× faster than kratos20's p50.
 *
 * We model HW-trap + kernel-entry cost with `kernel_entry_hw_trap`:
 *   - Touch a cache-line-cold slot in a per-CPU counter array (mimics
 *     perf_sw_event / mm_account_fault landing on an LLC-cold line in
 *     steady state).
 *   - One atomic-add-release (models the xchg on pt_regs save).
 *   - One cache-line-cold read from the IDT-shadow array (models the
 *     HW vector fetch — happens inside the CPU but its LLC pressure is
 *     real on a recently-idle system).
 */
struct TrapEntryState {
    uint64_t pf_count_user;
    uint64_t pf_count_kernel;
    uint64_t pf_count_write;
    uint64_t pf_count_instr;
};

/* Cache-cold scratch arrays for the HW-trap + kernel-entry model.  Both
   arrays are 4 MiB — larger than the per-core LLC slice (1.4 MiB on the
   kratos20 config) so a round-robin walk evicts each line before reuse.
   Each atomic counter is padded to a full cache line (PaddedAtomic) so
   the pcpu_counters array genuinely occupies 4 MiB of cache real estate,
   not just 512 KiB × 8 B of data surrounded by unused padding.  Earlier
   the counters were `std::atomic<uint64_t>[]` with no padding, which made
   the array footprint 8 × smaller than we thought and it all fit in L2 —
   so "cold-line" reads were ~no-ops.  */
/* PaddedAtomic moved to shared-primitives block near top of namespace. */

struct KernelEntryState {
    static constexpr size_t ARRAY_BYTES = 4 * 1024 * 1024;   /* 4 MiB */
    static constexpr size_t LINES       = ARRAY_BYTES / 64;  /* 65 536 */
    alignas(64) uint8_t idt_shadow[ARRAY_BYTES];       /* 4 MiB cold reads  */
    alignas(64) PaddedAtomic pcpu_counters[LINES];     /* 4 MiB cold atomics */
    uint64_t cursor = 0;
};

static inline void kernel_entry_hw_trap(KernelEntryState& k, uint64_t error_code) {
    uint64_t c = k.cursor++;
    uint64_t seed = c ^ error_code;

    /* 6 dep-chained cold loads against idt_shadow.  Split dispatch so
       the hot path has no per-iteration capture overhead. */
    static volatile uint64_t sink;
    if (g_capture_target) {
        sink = _cold_chain_reads_capturing(k.idt_shadow,
                                           KernelEntryState::LINES, seed, 6);
    } else {
        sink = _cold_chain_reads(k.idt_shadow,
                                 KernelEntryState::LINES, seed, 6);
    }

    const uint64_t mask = KernelEntryState::LINES - 1;
    if (g_capture_target) {
        capture_access(STREAM_KENTRY_IDT, /*op=read*/0, /*size_log2=6*/6,
                       static_cast<uint32_t>(seed));
        capture_access_vaddr(
            reinterpret_cast<uint64_t>(&k.pcpu_counters[(c * 1) & mask].val),
            /*op=rmw*/2, /*8B*/3, /*dep_flag=*/0);
    }
    k.pcpu_counters[(c * 1)   & mask].val.fetch_add(1, std::memory_order_release);
    if (g_capture_target) {
        capture_access_vaddr(
            reinterpret_cast<uint64_t>(&k.pcpu_counters[(c * 131) & mask].val),
            /*op=rmw*/2, /*8B*/3, /*dep_flag=*/0);
    }
    k.pcpu_counters[(c * 131) & mask].val.fetch_add(1, std::memory_order_release);
    if (g_capture_target) {
        capture_access(STREAM_KENTRY_PCPU, /*op=rmw*/2, /*size_log2=3*/3,
                       static_cast<uint32_t>(c));
    }
}

static inline void trap_entry(TrapEntryState& s, uint64_t error_code) {
    /* Linux: branchy decoding of error_code bits — cheap but not free. */
    const bool capturing = (g_capture_target != nullptr);
    if (error_code & 0x4) {
        if (capturing) {
            capture_access_vaddr(reinterpret_cast<uint64_t>(&s.pf_count_user),
                                 /*op=rmw*/2, /*8B*/3, /*dep=*/0);
        }
        s.pf_count_user++;
    } else {
        if (capturing) {
            capture_access_vaddr(reinterpret_cast<uint64_t>(&s.pf_count_kernel),
                                 /*op=rmw*/2, /*8B*/3, /*dep=*/0);
        }
        s.pf_count_kernel++;
    }
    if (error_code & 0x2) {
        if (capturing) {
            capture_access_vaddr(reinterpret_cast<uint64_t>(&s.pf_count_write),
                                 /*op=rmw*/2, /*8B*/3, /*dep=*/0);
        }
        s.pf_count_write++;
    }
    if (error_code & 0x10) {
        if (capturing) {
            capture_access_vaddr(reinterpret_cast<uint64_t>(&s.pf_count_instr),
                                 /*op=rmw*/2, /*8B*/3, /*dep=*/0);
        }
        s.pf_count_instr++;
    }
    /* irq_enable would be a real STI on x86; under SDE that's a NOP-ish.
       perf_sw_event is a counter increment, modelled by the above. */
    if (g_capture_target) {
        capture_access(STREAM_TRAP_SCRATCH, /*op=write*/1, /*size_log2=3*/3,
                       static_cast<uint32_t>(error_code));
    }
}

/* ---------- TLB sync imitation --------------------------------------------
 * Linux update_mmu_cache_range on x86 is ~no-op for new mappings (no TLB
 * invalidate needed for a fault that brought the page into existence).
 * For UP we just touch a counter; for SMP it would be a shootdown IPI.
 */
struct TlbSyncState {
    uint64_t tlb_flushes;
};

static inline void tlb_sync(TlbSyncState& s) {
    /* Even the no-shootdown path touches `mm->tlb_flush_pending` and a few
       barriers.  ~10-50 cycles. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (g_capture_target) {
        capture_access_vaddr(reinterpret_cast<uint64_t>(&s.tlb_flushes),
                             /*op=rmw*/2, /*8B*/3, /*dep=*/0);
        capture_access(STREAM_TLB_SCRATCH, /*op=write*/1, /*size_log2=3*/3, 0);
    }
    s.tlb_flushes++;
}

/* ---------- Bookkeeping imitation -----------------------------------------
 * Linux mm_account_fault + perf_sw_event + per-task counter updates.
 * ~30-80 cycles when warm.  Pack onto a single cache line so the cost
 * matches the warm-path Linux figure rather than cold-line overhead.
 */
struct BookkeepingState {
    std::atomic<uint64_t> task_min_flt;
    std::atomic<uint64_t> mm_pgfault;
    std::atomic<uint64_t> perf_count;
};

static inline void account_fault(BookkeepingState& s) {
    if (g_capture_target) {
        capture_access_vaddr(reinterpret_cast<uint64_t>(&s.task_min_flt),
                             /*op=rmw*/2, /*8B*/3, /*dep=*/0);
        capture_access_vaddr(reinterpret_cast<uint64_t>(&s.mm_pgfault),
                             /*op=rmw*/2, /*8B*/3, /*dep=*/0);
        capture_access_vaddr(reinterpret_cast<uint64_t>(&s.perf_count),
                             /*op=rmw*/2, /*8B*/3, /*dep=*/0);
        capture_access(STREAM_BK_COUNTERS, /*op=rmw*/2, /*size_log2=3*/3, 0);
    }
    s.task_min_flt.fetch_add(1, std::memory_order_relaxed);
    s.mm_pgfault.fetch_add(1, std::memory_order_relaxed);
    s.perf_count.fetch_add(1, std::memory_order_relaxed);
}

}  /* namespace LinuxPhase */

#pragma GCC pop_options

/* ---------- Phase 4 fast-replay body ------------------------------------
 * Reopen the LinuxPhase namespace OUTSIDE the -O0 pragma block.  The
 * replay body is emphatically NOT calibration-critical (cache-hit/miss
 * latency is charged by Sniper's perf model, not by our instruction
 * count), so we want the compiler to optimise it aggressively — fewer
 * simulated instructions per captured access means faster wall-clock
 * simulation.  Attribute optimize("O3") is belt-and-braces on top of
 * the file's ambient -O2 default.
 *
 * dep_flag accesses XOR the low 6 bits of a rolling `dep` accumulator
 * into the vaddr so Sniper sees a load-to-load dependency (same
 * serialisation as baseline's _cold_chain_reads).  The XOR stays
 * within the 64 B cache line so the target line is unchanged — the
 * dep is purely an instruction-level hint to the perf model, not a
 * cache-state change.
 */
namespace LinuxPhase {

static inline __attribute__((optimize("O3"), always_inline))
void replay_fault(const std::vector<CapturedAccess>& trace,
                  uint8_t* cold_page_ptr)
{
    uint64_t dep = 0xA5A5A5A5A5A5A5A5ULL;  /* arbitrary seed */
    for (const auto& acc : trace) {
        uint64_t addr = acc.vaddr;
        if (acc.dep_flag) {
            addr ^= (dep & 0x3F);
        }
        switch (acc.op) {
            case 0: {  /* read */
                volatile uint8_t v = *reinterpret_cast<volatile uint8_t*>(addr);
                dep ^= static_cast<uint64_t>(v);
                dep = (dep << 7) | (dep >> 57);
                break;
            }
            case 1: {  /* write */
                *reinterpret_cast<volatile uint8_t*>(addr) = 0;
                break;
            }
            case 2: {  /* rmw */
                __atomic_fetch_add(reinterpret_cast<uint8_t*>(addr),
                                   (uint8_t)1, __ATOMIC_RELAXED);
                break;
            }
            case 3: {  /* zero_sweep: cache-cold 4 KiB memset */
                if (cold_page_ptr) {
                    size_t n = (acc.size_log2 < 16) ? (1u << acc.size_log2) : 4096;
                    std::memset(cold_page_ptr, 0, n);
                }
                break;
            }
            default:
                break;
        }
    }
}

}  /* namespace LinuxPhase (O3 reopen) */
