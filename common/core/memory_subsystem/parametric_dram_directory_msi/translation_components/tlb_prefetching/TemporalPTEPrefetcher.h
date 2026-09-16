#pragma once

// ============================================================================
// TemporalPTEPrefetcher — TLB prefetcher that reads/writes temporal offset
// predictions from a shadow PTE payload word.
//
// Each virtual page has a small payload (up to 64 bits) that stores k signed
// delta-VPN entries with optional per-slot confidence counters.  On a demand
// miss the prefetcher decodes these deltas, predicts the next VPN(s) and
// issues PTW-transparent prefetches.  Optionally it *learns* by observing
// vpn transitions and writing observed deltas back into the source page's
// payload.
// ============================================================================

#include "tlb_prefetcher_base.h"
#include "debug_config.h"
#include "pte_offset_codec.h"
#include "pagetable.h"
#include "sim_log.h"
#include "mmu_base.h"
#include "base_filter.h"

#include "ConfidencePolicy.h"

#include <cstdint>
#include <list>
#include <memory>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

namespace ParametricDramDirectoryMSI
{

    // -----------------------------------------------------------------
    // Configuration enums
    // -----------------------------------------------------------------

    /** Learning mode: how (or whether) to update shadow PTE payloads. */
    enum class TemporalPTEMode
    {
        PTE_ONLY,                  ///< Read-only: decode offsets, never write back
        LEARN_PTE_ON_TRANSITIONS,  ///< Update prev VPN's payload with observed delta
        PC_COND_LEARN              ///< Like LEARN but slot selection uses PC hash
    };

    /** Victim selection when all slots are full and a new delta must be inserted. */
    enum class TemporalPTEReplacement
    {
        LOWEST_CONF,   ///< Replace slot with the lowest confidence
        LRU,           ///< (future) replace slot with highest age
        RANDOM         ///< Replace a random slot
    };

    // -----------------------------------------------------------------
    // Maximum number of offset slots (for fixed-size stat arrays)
    // -----------------------------------------------------------------
    static constexpr uint32_t TPTE_MAX_SLOTS = 8;

    // -----------------------------------------------------------------
    // Prefetcher class
    // -----------------------------------------------------------------

    class TemporalPTEPrefetcher : public TLBPrefetcherBase
    {
    public:
        TemporalPTEPrefetcher(Core *_core,
                              MemoryManagerBase *_memory_manager,
                              ShmemPerfModel *_shmem_perf_model,
                              String name,
                              // Codec configuration
                              uint32_t num_offsets,
                              uint32_t offset_bits,
                              uint32_t conf_bits,
                              uint32_t base_bit,
                              // Prefetch knobs
                              uint32_t conf_threshold,
                              uint32_t page_shift,
                              uint32_t region_shift,
                              // Learning
                              TemporalPTEMode mode,
                              TemporalPTEReplacement replacement,
                              uint32_t conf_init,
                              bool     enable_decay,
                              uint64_t decay_period,
                              // PC-conditioned tag bits (0 to disable)
                              uint32_t pc_tag_bits,
                              // PC table size (only for PC_COND_LEARN)
                              uint32_t pc_table_size,
                              // Reserve last slot for global (non-PC) delta
                              bool reserve_global_slot,
                              // Chained/recursive prefetching depth
                              uint32_t max_prefetch_depth,
                              // Path-score model for chained confidence gating
                              float chain_edge_decay,
                              float chain_score_threshold,
                              // Integer confidence floor for chained predictions (0 = disabled)
                              uint32_t chain_conf_threshold,
                              // Hit-path behavior
                              bool learn_on_hit,
                              bool prefetch_on_hit,
                              // Stride direct prefetch
                              uint32_t stride_conf_threshold,
                              bool stride_direct_prefetch,
                              uint32_t stride_direct_degree,
                              // Virtualized PC table
                              bool virtualize_pc_table,
                              // Confidence policy
                              const std::string& confidence_policy,
                              uint32_t conf_bump_amount,
                              uint32_t conf_decay_on_bump,
                              uint32_t conf_decay_on_miss,
                              // OS-managed side-table payload variant
                              bool     payload_in_side_table = false,
                              uint64_t side_table_base_pa = 0,
                              uint32_t side_table_payload_bits = 0,   // 0 = auto (codec width)
                              bool     side_table_radix = true,
                              uint32_t side_table_levels = 3,
                              uint32_t side_table_bits_per_level = 9,
                              uint32_t side_table_pwc_entries = 32,
                              // Model the DRAM writeback caused by dirtying the
                              // PTE cacheline on an in-PTE payload (delta) update.
                              bool     model_payload_writeback = false,
                              // Cache-only ablation: issue prefetch PTWs (warm the
                              // PTE lines in L2/LLC) but do NOT install the
                              // translation into the PQ/TLB.  false => demand
                              // accesses still walk, but hit warmed PTE lines.
                              bool     prefetch_install_pq = true);

        ~TemporalPTEPrefetcher() override;

        // ---- TLBPrefetcherBase interface ----
        std::vector<query_entry> performPrefetch(IntPtr address,
                                                 IntPtr eip,
                                                 Core::lock_signal_t lock,
                                                 bool modeled,
                                                 bool count,
                                                 PageTable *pt,
                                                 bool instruction = false,
                                                 bool tlb_hit = false,
                                                 bool pq_hit = false,
                                                 int page_size = 12) override;

        void notifyInstall(IntPtr address, int page_size) override;

        // Called by the MMU after the demand PTW completes: dirty the PTE
        // cacheline(s) whose in-PTE payload was updated this access, so the
        // cache models the resulting DRAM writeback traffic.
        void flushPendingPayloadWritebacks(PageTable *pt) override;

    private:
        // --- Codec ---
        PTEOffsetCodec m_codec;

        // --- Logging ---
        SimLog m_sim_log;

        // --- 2MB huge-page plane ---
        static constexpr uint32_t PAGE_SHIFT_2MB = 21;    ///< 2^21 = 2MB
        static constexpr uint64_t REGION_2MB_TAG = 1ULL << 63; ///< MSB tag to separate 2MB region_ids from 4KB in inflight/installed maps

        // --- Knobs ---
        uint32_t m_page_shift;
        uint32_t m_region_shift;          ///< log2(pages per region): 3→32KB, 4→64KB, 5→128KB
        uint32_t m_region_pages;          ///< 1 << m_region_shift (pages per region; may span multiple PTE cache lines)
        uint32_t m_conf_threshold;
        TemporalPTEMode m_mode;
        TemporalPTEReplacement m_replacement;
        uint32_t m_conf_init;
        bool     m_enable_decay;
        uint64_t m_decay_period;
        uint32_t m_pc_tag_bits;          ///< 0 = PC tagging disabled
        bool     m_reserve_global_slot;   ///< Reserve last slot for global delta (PC miss fallback)
        bool     m_current_is_pc_miss;    ///< Transient flag: true if current learn is from PC miss
        bool     m_current_is_instruction; ///< Transient flag: true if current access is instruction translation
        bool     m_learn_on_hit;           ///< If true, learn transitions on TLB hits too (not just misses)
        bool     m_prefetch_on_hit;        ///< If true, issue prefetches on TLB hits too (not just misses)

        // --- OS-managed side-table payload variant ---
        // When enabled, the temporal payload is fetched from a separate
        // OS-managed table in DRAM IN PARALLEL with the page-table walk, rather
        // than riding along in spare PTE bits.  Translation timing is unchanged;
        // the parallel access latency only delays when the prefetch is issued
        // (the materialization timestamp of every generated prefetch).
        bool     m_payload_in_side_table;  ///< Master enable for the side-table variant
        uint64_t m_side_table_base_pa;     ///< Physical base address of the reserved OS payload table (set lazily)
        uint64_t m_side_table_bytes;       ///< Reserved footprint of the OS payload table (set at reservation)
        uint32_t m_side_table_payload_bits;///< Bits stored per region entry; a 64B line packs 512/payload_bits payloads (0 in ctor → auto = codec width)
        bool     m_side_table_reserved;    ///< True once the region has been reserved from the OS allocator
        static constexpr uint64_t SIDE_TABLE_RESERVE_BYTES = 1ULL << 30; ///< 1 GB OS-managed table (reserved via handle_page_table_allocations, like HT/Cuckoo/HDC)

        // Radix "side-car" organization (Flavor A: independent multi-level radix
        // walked in parallel with the page table, with its own page-walk cache).
        bool     m_side_table_radix;          ///< true = radix walk; false = flat single-line lookup
        uint32_t m_side_table_levels;         ///< radix depth: (levels-1) pointer levels + 1 leaf
        uint32_t m_side_table_bits_per_level; ///< index bits consumed per level
        uint32_t m_side_table_pwc_entries;    ///< side-car page-walk-cache capacity (upper-level nodes)
        std::list<IntPtr> m_sidecar_pwc;              ///< LRU list of cached upper-level node PAs (MRU at front)
        std::unordered_set<IntPtr> m_sidecar_pwc_set; ///< membership index for m_sidecar_pwc

        // --- In-PTE payload-writeback modeling (model_payload_writeback) ---
        // When a delta is learned into an in-PTE payload, the PTE's cacheline is
        // dirtied so the cache's eviction path generates a realistic DRAM
        // writeback. The dirty-mark is DEFERRED to after the demand PTW (so the
        // line is resident): learnTransition() records the updated VPN here, and
        // flushPendingPayloadWritebacks() (called by the MMU post-walk) dirties it.
        bool m_model_payload_writeback;               ///< master enable (config knob)
        std::vector<std::pair<uint64_t,bool>> m_pending_payload_wb;  ///< {src_vpn, is_2mb} updated this access

        // Cache-only ablation: when false, prefetch PTWs still warm the PTE
        // lines in the data cache but no translation is installed into the PQ.
        bool m_prefetch_install_pq;

        /// Reserve the 1 GB OS payload table from the physical allocator (same
        /// mechanism the hash page tables use). Idempotent; runs on first use.
        void reserveSideTable();

        /// Walk the independent radix side-car for `line_no` (the leaf line that
        /// packs this region's payload), modeling per-level dependent cache
        /// accesses filtered by the side-car PWC.  Returns total walk latency.
        SubsecondTime modelSideTableRadixWalk(uint64_t line_no, IntPtr eip,
                                              Core::lock_signal_t lock, bool modeled, bool count);

        /// Model one parallel cache-line read to the OS payload-table entry for
        /// `region_id`, returning its access latency.  Does NOT advance the
        /// caller's clock (translation timing is unaffected).
        SubsecondTime modelSideTableAccess(uint64_t region_id, IntPtr eip,
                                           Core::lock_signal_t lock, bool modeled, bool count);

        // --- Stride direct prefetch ---
        uint32_t m_stride_conf_threshold;   ///< Consecutive matching deltas needed to trust a stride
        bool     m_stride_direct_prefetch;  ///< Issue PTWTransparent directly along detected stride
        uint32_t m_stride_direct_degree;    ///< How many stride-ahead pages to prefetch
        
        // --- Chained/recursive prefetching ---
        uint32_t m_max_prefetch_depth;   ///< Max recursion depth (0 = no chaining, 1+ = follow prefetched payloads)
        // Path-score model: deeper predictions decay multiplicatively along
        // the chain.  The full accumulated path confidence matters, not just
        // the local slot confidence.  Region fanout mildly penalizes aggressive
        // deep chains since each fan-out dilutes per-page accuracy.
        float    m_chain_edge_decay;     ///< Per-edge decay factor (0–1), e.g. 0.75
        float    m_chain_score_threshold;///< Minimum accumulated path score to issue a chained prediction
        float    m_chain_fanout_penalty; ///< Derived: 1/sqrt(region_pages), penalizes wide fan-out
        uint32_t m_chain_conf_threshold; ///< Integer confidence floor for depth > 0 (0 = disabled)

        // --- Confidence policy ---
        std::unique_ptr<ConfidencePolicy> m_confidence_policy;

        // --- Per-core state ---
        uint64_t m_last_region_id;       ///< Last region_id (vpn >> region_shift), for global fallback tracking
        uint64_t m_last_pc;
        uint64_t m_update_counter;       ///< Counts updates for decay period
        std::mt19937 m_rng;              ///< Per-core PRNG for RANDOM replacement (seeded by core_id)

        // --- Stride detector state (region-level, global fallback) ---
        int64_t  m_stride_last_delta;     ///< Previous region-to-region delta (global fallback)
        uint32_t m_stride_confidence;     ///< How many consecutive times the same delta repeated
        int64_t  m_stride_value;          ///< The detected stride (region delta)
        bool     m_stride_has_prev;       ///< True after the first region transition

        // PC → last_vpn table (direct-mapped, for PC_COND_LEARN mode)
        struct PCTableEntry {
            uint64_t pc_tag;
            uint64_t last_vpn;
            bool valid;
            int last_page_size;           ///< Page size of the last access (12=4KB, 21=2MB)
            // Per-PC stride detector state
            int64_t  stride_last_delta;
            uint32_t stride_confidence;
            int64_t  stride_value;
        };
        std::vector<PCTableEntry> m_pc_table;
        uint32_t m_pc_table_mask;        ///< (pc_table_size - 1) for indexing

        // Virtualized PC table: radix-tree backing store in emulated physical memory
        bool m_virtualize_pc_table;       ///< Enable backing store behind PC cache

        // Five-level radix tree (9 bits per level → 45-bit key space)
        // Each 512-entry frame fills one 4 KB page (512 × 8 B = 4 KB).
        // Mirrors a page-table-like walk: each level costs one emulated
        // memory access (PWC check + potential cache access).  Frames are
        // lazily allocated, so memory is proportional to unique PCs stored.
        static constexpr uint32_t PC_RADIX_BITS   = 9;
        static constexpr uint32_t PC_RADIX_FANOUT = 1u << PC_RADIX_BITS;  // 512
        static constexpr uint32_t PC_RADIX_MASK   = PC_RADIX_FANOUT - 1;
        static constexpr uint32_t PC_RADIX_LEVELS  = 5;

        // -----------------------------------------------------------------
        // Packed 64-bit layout for radix leaf entries (no pc_tag — position = key)
        //
        //   bits [35:0]  = last_vpn           (36 bits, unsigned)
        //   bit  [36]    = valid              ( 1 bit)
        //   bits [46:37] = stride_last_delta  (10 bits, signed two's complement)
        //   bits [50:47] = stride_confidence  ( 4 bits, unsigned)
        //   bits [60:51] = stride_value       (10 bits, signed two's complement)
        //   bits [63:61] = reserved           ( 3 bits)
        //                                     ------
        //                                     64 bits total
        // -----------------------------------------------------------------
        static uint64_t packPCData(const PCTableEntry& e);
        static void     unpackPCData(uint64_t packed, PCTableEntry& e);

        struct PCRadixFrame;
        struct PCRadixEntry {
            bool is_leaf;
            union {
                PCRadixFrame* next_level;                      // internal node slot
                struct { bool valid; uint64_t packed; } leaf;  // leaf node: 64-bit packed PCTableEntry
            } u;
        };
        struct PCRadixFrame {
            PCRadixEntry* entries;        ///< Dynamically allocated [PC_RADIX_FANOUT]
            IntPtr        emulated_ppn;   ///< Physical page in emulated memory
        };

        PCRadixFrame* m_pc_radix_root;

        PCRadixFrame* pcRadixAllocFrame(bool is_leaf_level);
        void          pcRadixInsert(uint64_t key, const PCTableEntry& entry);
        bool          pcRadixFind(uint64_t key, PCTableEntry& out);  ///< Returns true on hit, fills out
        void          pcRadixCleanup(PCRadixFrame* frame, uint32_t level);
        void          emitBackingStoreAccess(IntPtr emulated_ppn, uint32_t index, int pwc_level);

        // Transient context for backing store cache requests (set before resolvePCEntry)
        Core::lock_signal_t m_backing_lock;
        bool                m_backing_modeled;
        bool                m_backing_count;

        // Running count of valid entries in the radix backing store
        uint64_t m_pc_radix_entries_stored;

        // Tracking sets for unique coverage metrics (not registered as stats)
#if ENABLE_TPTE_UNIQUE_VPN_TRACKING
        std::unordered_set<uint64_t> m_unique_vpns_seen;
        std::unordered_set<uint64_t> m_unique_vpns_with_payload;
        std::unordered_set<uint64_t> m_unique_vpns_prefetched_for;
        std::unordered_set<uint64_t> m_unique_vpns_learned;
        std::unordered_set<uint64_t> m_unique_predicted_vpns;
#endif

        // -----------------------------------------------------------------
        // Prefetch timeliness tracking — inflight / installed maps
        //
        // Both maps are keyed by **region_id** (vpn >> m_region_shift) because
        // the prediction unit is a 32KB region (8 contiguous 4KB pages).
        //
        // Lifecycle:
        //   1. Region predicted (first PTWTransparent OK) → insert into m_inflight
        //   2. PQ materializes pages into TLB (notifyInstall) → move to m_installed
        //      (first page triggers the move; subsequent pages are no-ops)
        //   3. Demand access for region R:
        //      a) R in m_inflight → LATE (walk not yet installed)
        //      b) R in m_installed → INSTALLED (accurate prediction)
        //         - with pq_hit  → INSTALLED_AND_USEFUL (TLB still held page)
        //         - without pq_hit → INSTALLED_BUT_EVICTED (TLB evicted before demand)
        //      c) Neither → NOT_PREFETCHED (true miss)
        //   4. End of sim: remaining entries are unused prefetches
        // -----------------------------------------------------------------
        enum class PrefetchSource : uint8_t { TEMPORAL, STRIDE, CHAINED };

        struct InflightEntry {
            uint64_t issue_time_ns;       ///< When PTWTransparent was called
            uint64_t completion_time_ns;  ///< When PTW walk completed (query_entry.timestamp)
            uint64_t walk_latency_ns;     ///< completion_time_ns - issue_time_ns (walk cost)
            PrefetchSource source;        ///< Where this prediction came from
            uint32_t confidence;          ///< Confidence at prediction time
            uint32_t depth;               ///< Chain depth (0 = demand-triggered)
        };

        std::unordered_map<uint64_t, InflightEntry> m_inflight;   ///< region_id → pending prefetch (not yet in TLB)
        std::unordered_map<uint64_t, InflightEntry> m_installed;  ///< region_id → installed prefetch (in TLB, awaiting demand)

        // =====================================================================
        // Comprehensive Statistics
        // =====================================================================
        struct Stats {
            // --- 1. Invocation / high-level ---
            UInt64 queries;                         ///< Total performPrefetch() calls
            UInt64 queries_with_payload;             ///< Calls where PTE payload != 0
            UInt64 queries_without_payload;           ///< Calls where PTE payload == 0
            UInt64 queries_with_at_least_one_prefetch;///< Calls that produced ≥1 prefetch

            // --- 2. Prediction generation ---
            UInt64 predictions_issued;               ///< Total prefetches issued (successful PTWTransparent)
            UInt64 predictions_skipped_low_conf;     ///< Skipped: conf < threshold
            UInt64 predictions_skipped_zero_delta;   ///< Skipped: delta == 0
            UInt64 predictions_skipped_negative_vpn; ///< Skipped: predicted VPN ≤ 0
            UInt64 predictions_skipped_tlb_residency; ///< Skipped: >50% of region pages already in TLB

            // --- 3. PTW-transparent results ---
            UInt64 prefetch_attempts;                ///< Total PTWTransparent() calls
            UInt64 prefetch_successful;              ///< PTWTransparent returned valid PPN
            UInt64 prefetch_failed;                  ///< PTWTransparent returned PPN==0 (no mapping)

            // --- 4. Prefetch walk latency tracking (in femtoseconds) ---
            UInt64 total_prefetch_walk_latency_fs;  ///< Sum of all successful prefetch walk latencies (fs)
            UInt64 min_prefetch_walk_latency_fs;    ///< Minimum single prefetch walk latency (fs)
            UInt64 max_prefetch_walk_latency_fs;    ///< Maximum single prefetch walk latency (fs)

            // --- 5. Confidence analysis (at prediction time) ---
            UInt64 sum_conf_at_prediction;           ///< Sum of confidence values of issued predictions
            UInt64 max_conf_at_prediction;           ///< Max confidence seen at prediction time
            UInt64 sum_conf_all_slots;               ///< Sum of all decoded slot confidences (every query)
            UInt64 total_slots_decoded;              ///< Total slots decoded across all queries
            UInt64 slots_with_nonzero_conf;          ///< Slots decoded with conf > 0
            UInt64 slots_with_nonzero_delta;         ///< Slots decoded with delta != 0

            // --- 6. Delta magnitude analysis ---
            UInt64 sum_abs_delta_predicted;           ///< Sum of |delta| for issued predictions
            UInt64 max_abs_delta_predicted;           ///< Max |delta| seen at prediction time
            UInt64 positive_delta_predictions;       ///< Predictions with delta > 0
            UInt64 negative_delta_predictions;       ///< Predictions with delta < 0

            // --- 7. Learning / update path ---
            UInt64 learning_opportunities;           ///< Transitions where src_vpn != 0 && src_vpn != vpn
            UInt64 learning_updates;                 ///< Payload writes (bumps + inserts)
            UInt64 learning_bumps;                   ///< Existing delta confidence bumped
            UInt64 learning_inserts;                 ///< New delta inserted (victim replaced)
            UInt64 learning_delta_out_of_range;      ///< Transitions skipped: delta doesn't fit
            UInt64 learning_same_page;               ///< Transitions skipped: src_vpn == vpn
            UInt64 decay_events;                     ///< Number of confidence decay sweeps (periodic)
            UInt64 competitive_decay_on_bump;        ///< Competitive decay events triggered by a bump (others decayed)
            UInt64 competitive_decay_on_miss;         ///< Competitive decay events triggered by a new delta (all decayed)

            // --- 8. Per-slot utilization ---
            UInt64 slot_prediction_count[TPTE_MAX_SLOTS];  ///< Predictions issued per slot index
            UInt64 slot_bump_count[TPTE_MAX_SLOTS];        ///< Confidence bumps per slot index
            UInt64 slot_replace_count[TPTE_MAX_SLOTS];     ///< Replacements per slot index

            // --- 9. Unique page coverage ---
            UInt64 unique_vpns_seen;                 ///< Distinct VPNs passed to performPrefetch
            UInt64 unique_vpns_with_payload;         ///< Distinct VPNs with non-zero payload
            UInt64 unique_vpns_prefetched_for;       ///< Distinct VPNs that triggered ≥1 prefetch
            UInt64 unique_vpns_learned;              ///< Distinct VPNs whose payload was updated
            UInt64 unique_predicted_vpns;            ///< Distinct predicted target VPNs

            // --- 10. PC table (PC_COND_LEARN mode) ---
            UInt64 pc_table_hits;                    ///< PC table lookups that found a valid match
            UInt64 pc_table_misses;                  ///< PC table lookups with tag mismatch or invalid
            UInt64 pc_table_evictions;               ///< PC table entries overwritten (tag changed)

            // --- 10c. Virtualized PC backing store ---
            UInt64 pc_backing_hits;                  ///< Cache miss resolved by backing store
            UInt64 pc_backing_misses;                ///< Cold miss (not in cache or backing)
            UInt64 pc_backing_writebacks;            ///< Cache evictions written back to backing
            UInt64 pc_radix_frames_allocated;        ///< Radix frames allocated in emulated physical memory
            UInt64 pc_radix_entries_stored;          ///< Current valid entries in radix backing store
            UInt64 pc_radix_entries_peak;             ///< Peak valid entries in radix backing store
            UInt64 pc_radix_bytes;                    ///< Total emulated memory (frames × 4096)
            UInt64 pc_backing_cache_accesses;        ///< Cache accesses issued for backing store reads/writes
            UInt64 pc_backing_cache_l1d;              ///< Backing store accesses that hit in L1D
            UInt64 pc_backing_cache_l2;               ///< Backing store accesses that hit in L2
            UInt64 pc_backing_cache_nuca;             ///< Backing store accesses that hit in NUCA (LLC)
            UInt64 pc_backing_cache_dram;             ///< Backing store accesses that went to DRAM
            UInt64 pc_backing_total_latency_fs;       ///< Total latency (femtoseconds) of backing store cache accesses
            UInt64 pc_backing_pwc_hits;              ///< Backing store accesses absorbed by PWC
            
            // --- 10b. Global slot stats (when reserve_global_slot enabled) ---
            UInt64 global_slot_updates;              ///< Updates to the reserved global slot
            UInt64 global_slot_bumps;                ///< Confidence bumps on global slot
            UInt64 global_slot_predictions;          ///< Predictions issued from global slot

            // --- 11. Region-level prefetching ---
            UInt64 region_predictions_issued;         ///< Number of region predictions (each fans out to region_pages walks)
            UInt64 region_pages_prefetched;           ///< Total individual pages prefetched via region fan-out
            UInt64 region_pages_failed;               ///< Individual page prefetches within a region that failed

            // --- 12. Inflight lifecycle ---
            UInt64 demand_accesses;                   ///< Total demand accesses (performPrefetch calls)
            UInt64 inflight_inserted;                 ///< Total prefetches added to inflight map
            UInt64 inflight_overwrites;               ///< Re-prefetches for VPN already in inflight
            UInt64 inflight_installed;                ///< Moved to installed via notifyInstall (TLB materialization)
            UInt64 inflight_at_end;                   ///< Entries remaining in inflight at end of simulation
            UInt64 inflight_high_water_mark;          ///< Peak inflight count
            UInt64 installed_at_end;                  ///< Entries remaining in installed at end of simulation
            UInt64 installed_overwrites;              ///< Re-install for region already in installed (useless prefetch replaced)
            UInt64 installed_high_water_mark;         ///< Peak installed map size

            // --- 13. Demand-side accuracy / coverage ---
            UInt64 demand_hit_installed;              ///< Demand hit on installed prefetch AND pq_hit (useful + timely)
            UInt64 demand_hit_installed_evicted;      ///< Demand hit on installed prefetch but NOT pq_hit (accurate but TLB evicted)
            UInt64 demand_hit_inflight;               ///< Demand hit on VPN still inflight (late — walk not yet in TLB)
            UInt64 demand_miss_not_prefetched;        ///< True miss: VPN was never prefetched

            // --- 13a. Per-source demand hits ---
            UInt64 demand_hit_installed_temporal;     ///< Installed hits from temporal delta predictions
            UInt64 demand_hit_installed_stride;       ///< Installed hits from stride predictions
            UInt64 demand_hit_installed_chained;      ///< Installed hits from chained predictions
            UInt64 demand_hit_inflight_temporal;      ///< Late hits from temporal delta predictions
            UInt64 demand_hit_inflight_stride;        ///< Late hits from stride predictions
            UInt64 demand_hit_inflight_chained;       ///< Late hits from chained predictions

            // --- 13c. Lead time distribution (installed hits) ---
            UInt64 sum_completion_to_demand_ns;       ///< Sum of (demand_time - completion_time) for installed hits
            UInt64 max_completion_to_demand_ns;       ///< Max (demand_time - completion_time) for installed hits

            // --- 13d. Late gap distribution (inflight hits) ---
            UInt64 sum_inflight_remaining_ns;         ///< Sum of (completion_time - demand_time) for late hits (how much walk time remains)
            UInt64 max_inflight_remaining_ns;         ///< Max remaining walk time for a late hit

            // --- 13e. Latency saved (Stat 1) ---
            UInt64 sum_walk_latency_saved_installed_ns;   ///< Sum of walk latency avoided for installed hits (no PTW performed)
            UInt64 sum_walk_latency_saved_inflight_ns;    ///< Sum of remaining walk time for inflight hits (partial savings)

            // --- 13f. Timeliness histogram (Stat 3) ---
            // Installed lead time buckets: [0,1us), [1us,10us), [10us,100us), [100us,1ms), [1ms,inf)
            static constexpr uint32_t NUM_TIMELINESS_BUCKETS = 5;
            UInt64 installed_lead_time_bucket[NUM_TIMELINESS_BUCKETS];
            // Inflight remaining time buckets: same ranges
            UInt64 inflight_remaining_bucket[NUM_TIMELINESS_BUCKETS];

            // --- 15. Chained/recursive prefetching (by depth) ---
            static constexpr uint32_t MAX_CHAIN_DEPTH = 8;  ///< Max supported chain depth for stats
            UInt64 depth_predictions[MAX_CHAIN_DEPTH];       ///< Predictions issued at each depth (0=demand-triggered)

            // --- 13b. Per-depth accuracy (placed after MAX_CHAIN_DEPTH declaration) ---
            UInt64 depth_inflight_inserted[MAX_CHAIN_DEPTH]; ///< Prefetches inserted at each depth
            UInt64 depth_demand_hit_installed[MAX_CHAIN_DEPTH]; ///< Installed hits at each depth
            UInt64 depth_demand_hit_inflight[MAX_CHAIN_DEPTH];  ///< Late hits at each depth
            UInt64 depth_pages_prefetched[MAX_CHAIN_DEPTH];  ///< Pages prefetched at each depth
            UInt64 depth_pages_failed[MAX_CHAIN_DEPTH];      ///< Failed prefetches at each depth
            UInt64 chained_predictions_total;                ///< Total chained predictions (depth > 0)
            UInt64 chained_prefetch_opportunities;           ///< Number of times we could chain (had payload)
            UInt64 chained_prefetch_skipped_max_depth;       ///< Skipped due to max depth reached
            
            // --- 16. Instruction vs Data breakdown ---
            UInt64 queries_instruction;                      ///< performPrefetch() calls triggered by instruction translations
            UInt64 queries_data;                             ///< performPrefetch() calls triggered by data translations
            UInt64 queries_with_payload_instruction;         ///< Instruction queries where PTE payload != 0
            UInt64 queries_with_payload_data;                ///< Data queries where PTE payload != 0
            UInt64 predictions_issued_instruction;           ///< Prefetches issued for instruction translations
            UInt64 predictions_issued_data;                  ///< Prefetches issued for data translations
            UInt64 prefetch_successful_instruction;          ///< Successful PTWTransparent for instruction translations
            UInt64 prefetch_successful_data;                 ///< Successful PTWTransparent for data translations
            UInt64 prefetch_failed_instruction;              ///< Failed PTWTransparent for instruction translations
            UInt64 prefetch_failed_data;                     ///< Failed PTWTransparent for data translations
            UInt64 learning_opportunities_instruction;       ///< Learning transitions triggered by instruction accesses
            UInt64 learning_opportunities_data;              ///< Learning transitions triggered by data accesses
            UInt64 learning_updates_instruction;             ///< Payload writes triggered by instruction accesses
            UInt64 learning_updates_data;                    ///< Payload writes triggered by data accesses
            UInt64 total_prefetch_walk_latency_fs_instruction; ///< Walk latency sum for instruction-triggered prefetches (fs)
            UInt64 total_prefetch_walk_latency_fs_data;      ///< Walk latency sum for data-triggered prefetches (fs)

            // --- 17. Stride ---
            UInt64 stride_detected;                   ///< Times a stride was confirmed (confidence reached threshold)
            UInt64 stride_changed;                    ///< Times detected stride value changed
            UInt64 stride_reset;                      ///< Times confidence was reset (delta mismatch)

            // --- 18. Stride direct prefetch ---
            UInt64 stride_direct_issued;              ///< Total direct stride prefetch walks issued
            UInt64 stride_direct_successful;          ///< Direct stride walks that returned valid PPN
            UInt64 stride_direct_failed;              ///< Direct stride walks that returned PPN==0

            // --- 19. 2MB huge-page plane ---
            UInt64 queries_2mb;                       ///< Demand accesses with 2MB page size
            UInt64 predictions_2mb;                   ///< Predictions issued at 2MB granularity
            UInt64 prefetch_successful_2mb;            ///< Successful 2MB prefetches
            UInt64 learning_transitions_2mb;           ///< Learning transitions where both src+dst are 2MB
            UInt64 learning_skipped_mixed_pagesize;   ///< Transitions skipped due to mixed 4KB/2MB

            // --- 20. OS-managed side-table access (payload_in_side_table) ---
            UInt64 side_table_accesses;               ///< Parallel side-table reads modeled
            UInt64 side_table_total_latency_fs;       ///< Sum of side-table access latencies (femtoseconds)
            UInt64 side_table_min_latency_fs;         ///< Min single side-table access latency (fs)
            UInt64 side_table_max_latency_fs;         ///< Max single side-table access latency (fs)
            UInt64 side_table_l1d;                    ///< Side-table reads that hit in L1D
            UInt64 side_table_l2;                     ///< ... hit in L2
            UInt64 side_table_nuca;                   ///< ... hit in NUCA/LLC
            UInt64 side_table_dram;                   ///< ... went to DRAM
            // radix side-car
            UInt64 side_table_pwc_hits;               ///< Upper-level nodes absorbed by the side-car PWC
            UInt64 side_table_pwc_misses;             ///< Upper-level nodes that missed the PWC (→ memory access)
            UInt64 side_table_levels_accessed;        ///< Total radix levels that issued a memory access (sum over walks)

            // --- 21. In-PTE payload writeback modeling (model_payload_writeback) ---
            UInt64 payload_writes;                    ///< Payload (delta) updates that requested a PTE-line dirty
            UInt64 payload_dirty_marks_hit;           ///< Updates where the PTE line was resident and newly dirtied
            UInt64 payload_dirty_marks_miss;          ///< Updates where the PTE line was not resident (no writeback modeled)
            UInt64 payload_dirty_marks_coalesced;     ///< Updates where the PTE line was already dirty (write absorbed)
        } m_stats;

        // --- Helpers ---
        void insertInflight(uint64_t region_id, uint64_t issue_time_ns, uint64_t completion_time_ns,
                            PrefetchSource source, uint32_t confidence, uint32_t depth);
        void learnTransition(uint64_t src_vpn, uint64_t curr_vpn, IntPtr eip, PageTable *pt);
        std::vector<query_entry> strideDirectPrefetch(uint64_t vpn, int64_t stride_pages,
                                                      uint32_t stride_confidence,
                                                      IntPtr eip, Core::lock_signal_t lock,
                                                      bool modeled, bool count, PageTable *pt);
        PCTableEntry& resolvePCEntry(IntPtr eip);  ///< Cache + backing-store lookup with write-back
        uint32_t pcHash(IntPtr eip) const;
        uint64_t pcTag(IntPtr eip) const;
        void registerAllStats(core_id_t core_id);
        void finalizeUniquePageStats();
    };

} // namespace ParametricDramDirectoryMSI
