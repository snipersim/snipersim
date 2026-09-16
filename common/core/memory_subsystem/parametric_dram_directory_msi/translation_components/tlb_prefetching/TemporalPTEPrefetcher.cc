// ============================================================================
// TemporalPTEPrefetcher — Implementation
// ============================================================================
//
// OVERVIEW
// --------
// This prefetcher predicts future TLB misses by storing small delta-encoded
// successor hints ("offsets") directly inside the page table entry (PTE)
// payload.  Each virtual page's PTE carries k signed delta-region entries
// with per-slot confidence counters (e.g., 4 slots × 18-bit delta × 2-bit
// confidence = 80 bits, fitting within spare PTE cache-line bits).
//
// On a TLB miss the prefetcher:
//   1. Reads the payload from the demand page's PTE
//   2. Decodes the delta/confidence slots
//   3. For each slot above the confidence threshold, predicts a target
//      region and issues page-table-transparent prefetch walks for every
//      page in that region
//   4. Optionally chains deeper: reads the predicted region's payload and
//      repeats (BFS up to max_prefetch_depth levels)
//   5. Learns by observing region-to-region transitions and writing the
//      observed delta back into the source region's PTE payload
//
// KEY ABSTRACTIONS
// ----------------
// - **Region**: a group of 2^region_shift contiguous 4KB pages (e.g.,
//   region_shift=3 → 8 pages = 32KB).  All predictions and learning
//   operate at region granularity.  A PTE cache line (64B) holds exactly
//   8 PTEs, so region_shift=3 means one modeled cache-line walk per
//   predicted region.
//
// - **Payload codec** (PTEOffsetCodec): encodes/decodes delta+confidence
//   slots into/from a 128-bit PTE payload word.  Deltas are signed
//   (two's complement) and measured in region units.
//
// - **PC-conditioned learning** (PC_COND_LEARN mode): instead of a single
//   global "last region" tracker, each unique instruction PC gets its own
//   last-region history via a direct-mapped PC table.  This prevents
//   interleaved access streams from polluting each other's delta patterns.
//
// - **Virtualized PC table**: an optional radix-tree backing store in
//   emulated physical memory.  On PC table eviction the entry is written
//   back; on miss the backing store is probed and the entry promoted.
//   This models a realistic SW/HW co-design where a small SRAM cache
//   sits in front of a page-table-like structure.
//
// - **Confidence policies** (ConfidencePolicy): pluggable strategies for
//   how confidence counters are updated during learning (none, competitive
//   linear decay, exponential halving, bump-only-strong).
//
// - **Stride detector**: per-PC (or global) page-level stride detection.
//   When a stable delta repeats stride_conf_threshold times, direct
//   PTWTransparent walks are issued along the stride.
//
// - **Timeliness tracking**: inflight/installed maps track whether demand
//   arrives before or after a prefetched region is installed in the TLB,
//   enabling accuracy and lead-time statistics.
//
// CALL FLOW (demand miss)
// -----------------------
//   TLB::lookup(miss) → PrefetchQueue::performPrefetchers()
//     → TemporalPTEPrefetcher::performPrefetch()
//       → [timeliness check] → [read payload] → [decode & predict]
//         → PTWTransparent() per page → [inflight tracking]
//         → [chained: enqueue deeper levels]
//       → [learning: learnTransition()]
//       → [stride detection: strideDirectPrefetch()]
//     ← vector<query_entry> (prefetched translations for PQ insertion)
//
// ============================================================================

#include "TemporalPTEPrefetcher.h"
#include "debug_config.h"
#include "stats.h"
#include "simulator.h"
#include "mimicos.h"
#include "shmem_perf_model.h"
#include "../tlb.h"
#include "mmu_base.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>

namespace ParametricDramDirectoryMSI
{

// ============================================================================
// Stats registration helper
// ============================================================================

void TemporalPTEPrefetcher::registerAllStats(core_id_t core_id)
{
    const char *cat = "tlb_temporal_pte";

    // 1. Invocation / high-level
    registerStatsMetric(cat, core_id, "queries",                             &m_stats.queries);
    registerStatsMetric(cat, core_id, "queries_with_payload",                &m_stats.queries_with_payload);
    registerStatsMetric(cat, core_id, "queries_without_payload",             &m_stats.queries_without_payload);
    registerStatsMetric(cat, core_id, "queries_with_at_least_one_prefetch",  &m_stats.queries_with_at_least_one_prefetch);

    // 2. Prediction generation
    registerStatsMetric(cat, core_id, "predictions_issued",                  &m_stats.predictions_issued);
    registerStatsMetric(cat, core_id, "predictions_skipped_low_conf",        &m_stats.predictions_skipped_low_conf);
    registerStatsMetric(cat, core_id, "predictions_skipped_zero_delta",      &m_stats.predictions_skipped_zero_delta);
    registerStatsMetric(cat, core_id, "predictions_skipped_negative_vpn",    &m_stats.predictions_skipped_negative_vpn);
    registerStatsMetric(cat, core_id, "predictions_skipped_tlb_residency",   &m_stats.predictions_skipped_tlb_residency);

    // 3. PTW-transparent results
    registerStatsMetric(cat, core_id, "prefetch_attempts",                   &m_stats.prefetch_attempts);
    registerStatsMetric(cat, core_id, "prefetch_successful",                 &m_stats.prefetch_successful);
    registerStatsMetric(cat, core_id, "prefetch_failed",                     &m_stats.prefetch_failed);

    // 4. Walk latency (femtoseconds, divide by 1e6 for ns)
    registerStatsMetric(cat, core_id, "total_prefetch_walk_latency_fs",      &m_stats.total_prefetch_walk_latency_fs);
    registerStatsMetric(cat, core_id, "min_prefetch_walk_latency_fs",        &m_stats.min_prefetch_walk_latency_fs);
    registerStatsMetric(cat, core_id, "max_prefetch_walk_latency_fs",        &m_stats.max_prefetch_walk_latency_fs);

    // 5. Confidence analysis
    registerStatsMetric(cat, core_id, "sum_conf_at_prediction",              &m_stats.sum_conf_at_prediction);
    registerStatsMetric(cat, core_id, "max_conf_at_prediction",              &m_stats.max_conf_at_prediction);
    registerStatsMetric(cat, core_id, "sum_conf_all_slots",                  &m_stats.sum_conf_all_slots);
    registerStatsMetric(cat, core_id, "total_slots_decoded",                 &m_stats.total_slots_decoded);
    registerStatsMetric(cat, core_id, "slots_with_nonzero_conf",             &m_stats.slots_with_nonzero_conf);
    registerStatsMetric(cat, core_id, "slots_with_nonzero_delta",            &m_stats.slots_with_nonzero_delta);

    // 5. Delta magnitude analysis
    registerStatsMetric(cat, core_id, "sum_abs_delta_predicted",             &m_stats.sum_abs_delta_predicted);
    registerStatsMetric(cat, core_id, "max_abs_delta_predicted",             &m_stats.max_abs_delta_predicted);
    registerStatsMetric(cat, core_id, "positive_delta_predictions",          &m_stats.positive_delta_predictions);
    registerStatsMetric(cat, core_id, "negative_delta_predictions",          &m_stats.negative_delta_predictions);

    // 6. Learning / update path
    registerStatsMetric(cat, core_id, "learning_opportunities",              &m_stats.learning_opportunities);
    registerStatsMetric(cat, core_id, "learning_updates",                    &m_stats.learning_updates);
    registerStatsMetric(cat, core_id, "learning_bumps",                      &m_stats.learning_bumps);
    registerStatsMetric(cat, core_id, "learning_inserts",                    &m_stats.learning_inserts);
    registerStatsMetric(cat, core_id, "learning_delta_out_of_range",         &m_stats.learning_delta_out_of_range);
    registerStatsMetric(cat, core_id, "learning_same_page",                  &m_stats.learning_same_page);
    registerStatsMetric(cat, core_id, "decay_events",                        &m_stats.decay_events);
    registerStatsMetric(cat, core_id, "competitive_decay_on_bump",           &m_stats.competitive_decay_on_bump);
    registerStatsMetric(cat, core_id, "competitive_decay_on_miss",           &m_stats.competitive_decay_on_miss);

    // 7. Per-slot utilization
    uint32_t nslots = std::min(m_codec.config().num_entries, TPTE_MAX_SLOTS);
    for (uint32_t s = 0; s < nslots; s++)
    {
        String suffix = String("slot") + itostr(s);
        registerStatsMetric(cat, core_id, (suffix + "_predictions").c_str(), &m_stats.slot_prediction_count[s]);
        registerStatsMetric(cat, core_id, (suffix + "_bumps").c_str(),       &m_stats.slot_bump_count[s]);
        registerStatsMetric(cat, core_id, (suffix + "_replaces").c_str(),    &m_stats.slot_replace_count[s]);
    }

    // 8. Unique page coverage
#if ENABLE_TPTE_UNIQUE_VPN_TRACKING
    registerStatsMetric(cat, core_id, "unique_vpns_seen",                    &m_stats.unique_vpns_seen);
    registerStatsMetric(cat, core_id, "unique_vpns_with_payload",            &m_stats.unique_vpns_with_payload);
    registerStatsMetric(cat, core_id, "unique_vpns_prefetched_for",          &m_stats.unique_vpns_prefetched_for);
    registerStatsMetric(cat, core_id, "unique_vpns_learned",                 &m_stats.unique_vpns_learned);
    registerStatsMetric(cat, core_id, "unique_predicted_vpns",               &m_stats.unique_predicted_vpns);
#endif

    // 9. PC table (PC_COND_LEARN mode)
    registerStatsMetric(cat, core_id, "pc_table_hits",                       &m_stats.pc_table_hits);
    registerStatsMetric(cat, core_id, "pc_table_misses",                     &m_stats.pc_table_misses);
    registerStatsMetric(cat, core_id, "pc_table_evictions",                  &m_stats.pc_table_evictions);

    // 9c. Virtualized PC backing store
    registerStatsMetric(cat, core_id, "pc_backing_hits",                     &m_stats.pc_backing_hits);
    registerStatsMetric(cat, core_id, "pc_backing_misses",                   &m_stats.pc_backing_misses);
    registerStatsMetric(cat, core_id, "pc_backing_writebacks",               &m_stats.pc_backing_writebacks);
    registerStatsMetric(cat, core_id, "pc_radix_frames_allocated",           &m_stats.pc_radix_frames_allocated);
    registerStatsMetric(cat, core_id, "pc_radix_entries_stored",             &m_stats.pc_radix_entries_stored);
    registerStatsMetric(cat, core_id, "pc_radix_entries_peak",               &m_stats.pc_radix_entries_peak);
    registerStatsMetric(cat, core_id, "pc_radix_bytes",                      &m_stats.pc_radix_bytes);
    registerStatsMetric(cat, core_id, "pc_backing_cache_accesses",           &m_stats.pc_backing_cache_accesses);
    registerStatsMetric(cat, core_id, "pc_backing_cache_l1d",                &m_stats.pc_backing_cache_l1d);
    registerStatsMetric(cat, core_id, "pc_backing_cache_l2",                 &m_stats.pc_backing_cache_l2);
    registerStatsMetric(cat, core_id, "pc_backing_cache_nuca",               &m_stats.pc_backing_cache_nuca);
    registerStatsMetric(cat, core_id, "pc_backing_cache_dram",               &m_stats.pc_backing_cache_dram);
    registerStatsMetric(cat, core_id, "pc_backing_total_latency_fs",         &m_stats.pc_backing_total_latency_fs);
    registerStatsMetric(cat, core_id, "pc_backing_pwc_hits",                 &m_stats.pc_backing_pwc_hits);

    // 9b. Global slot stats (when reserve_global_slot enabled)
    registerStatsMetric(cat, core_id, "global_slot_updates",                 &m_stats.global_slot_updates);
    registerStatsMetric(cat, core_id, "global_slot_bumps",                   &m_stats.global_slot_bumps);
    registerStatsMetric(cat, core_id, "global_slot_predictions",             &m_stats.global_slot_predictions);

    // 10. Region-level prefetching
    registerStatsMetric(cat, core_id, "region_predictions_issued",           &m_stats.region_predictions_issued);
    registerStatsMetric(cat, core_id, "region_pages_prefetched",             &m_stats.region_pages_prefetched);
    registerStatsMetric(cat, core_id, "region_pages_failed",                 &m_stats.region_pages_failed);

    // 11. Inflight lifecycle
    registerStatsMetric(cat, core_id, "demand_accesses",                     &m_stats.demand_accesses);
    registerStatsMetric(cat, core_id, "inflight_inserted",                   &m_stats.inflight_inserted);
    registerStatsMetric(cat, core_id, "inflight_overwrites",                 &m_stats.inflight_overwrites);
    registerStatsMetric(cat, core_id, "inflight_installed",                  &m_stats.inflight_installed);
    registerStatsMetric(cat, core_id, "inflight_at_end",                     &m_stats.inflight_at_end);
    registerStatsMetric(cat, core_id, "inflight_high_water_mark",            &m_stats.inflight_high_water_mark);
    registerStatsMetric(cat, core_id, "installed_at_end",                    &m_stats.installed_at_end);
    registerStatsMetric(cat, core_id, "installed_overwrites",               &m_stats.installed_overwrites);
    registerStatsMetric(cat, core_id, "installed_high_water_mark",          &m_stats.installed_high_water_mark);

    // 12. Demand-side accuracy / coverage
    registerStatsMetric(cat, core_id, "demand_hit_installed",                &m_stats.demand_hit_installed);
    registerStatsMetric(cat, core_id, "demand_hit_installed_evicted",        &m_stats.demand_hit_installed_evicted);
    registerStatsMetric(cat, core_id, "demand_hit_inflight",                 &m_stats.demand_hit_inflight);
    registerStatsMetric(cat, core_id, "demand_miss_not_prefetched",          &m_stats.demand_miss_not_prefetched);

    // 12a. Per-source demand hits
    registerStatsMetric(cat, core_id, "demand_hit_installed_temporal",       &m_stats.demand_hit_installed_temporal);
    registerStatsMetric(cat, core_id, "demand_hit_installed_stride",         &m_stats.demand_hit_installed_stride);
    registerStatsMetric(cat, core_id, "demand_hit_installed_chained",        &m_stats.demand_hit_installed_chained);
    registerStatsMetric(cat, core_id, "demand_hit_inflight_temporal",        &m_stats.demand_hit_inflight_temporal);
    registerStatsMetric(cat, core_id, "demand_hit_inflight_stride",          &m_stats.demand_hit_inflight_stride);
    registerStatsMetric(cat, core_id, "demand_hit_inflight_chained",         &m_stats.demand_hit_inflight_chained);

    // 12b. Per-depth accuracy
    for (uint32_t d = 0; d < Stats::MAX_CHAIN_DEPTH; d++)
    {
        String suffix = String("depth") + itostr(d);
        registerStatsMetric(cat, core_id, (suffix + "_inflight_inserted").c_str(),    &m_stats.depth_inflight_inserted[d]);
        registerStatsMetric(cat, core_id, (suffix + "_demand_hit_installed").c_str(),  &m_stats.depth_demand_hit_installed[d]);
        registerStatsMetric(cat, core_id, (suffix + "_demand_hit_inflight").c_str(),   &m_stats.depth_demand_hit_inflight[d]);
    }

    // 12c. Lead time distribution (installed hits)
    registerStatsMetric(cat, core_id, "sum_completion_to_demand_ns",         &m_stats.sum_completion_to_demand_ns);
    registerStatsMetric(cat, core_id, "max_completion_to_demand_ns",         &m_stats.max_completion_to_demand_ns);

    // 12d. Late gap distribution (inflight hits)
    registerStatsMetric(cat, core_id, "sum_inflight_remaining_ns",           &m_stats.sum_inflight_remaining_ns);
    registerStatsMetric(cat, core_id, "max_inflight_remaining_ns",           &m_stats.max_inflight_remaining_ns);

    // 12e. Latency saved (Stat 1)
    registerStatsMetric(cat, core_id, "sum_walk_latency_saved_installed_ns", &m_stats.sum_walk_latency_saved_installed_ns);
    registerStatsMetric(cat, core_id, "sum_walk_latency_saved_inflight_ns",  &m_stats.sum_walk_latency_saved_inflight_ns);

    // 12f. Timeliness histogram (Stat 3)
    registerStatsMetric(cat, core_id, "installed_lead_lt1us",                &m_stats.installed_lead_time_bucket[0]);
    registerStatsMetric(cat, core_id, "installed_lead_1us_10us",             &m_stats.installed_lead_time_bucket[1]);
    registerStatsMetric(cat, core_id, "installed_lead_10us_100us",           &m_stats.installed_lead_time_bucket[2]);
    registerStatsMetric(cat, core_id, "installed_lead_100us_1ms",            &m_stats.installed_lead_time_bucket[3]);
    registerStatsMetric(cat, core_id, "installed_lead_ge1ms",                &m_stats.installed_lead_time_bucket[4]);
    registerStatsMetric(cat, core_id, "inflight_remaining_lt1us",            &m_stats.inflight_remaining_bucket[0]);
    registerStatsMetric(cat, core_id, "inflight_remaining_1us_10us",         &m_stats.inflight_remaining_bucket[1]);
    registerStatsMetric(cat, core_id, "inflight_remaining_10us_100us",       &m_stats.inflight_remaining_bucket[2]);
    registerStatsMetric(cat, core_id, "inflight_remaining_100us_1ms",        &m_stats.inflight_remaining_bucket[3]);
    registerStatsMetric(cat, core_id, "inflight_remaining_ge1ms",            &m_stats.inflight_remaining_bucket[4]);

    // 14. Chained/recursive prefetching
    for (uint32_t d = 0; d < Stats::MAX_CHAIN_DEPTH; d++)
    {
        String suffix = String("depth") + itostr(d);
        registerStatsMetric(cat, core_id, (suffix + "_predictions").c_str(),      &m_stats.depth_predictions[d]);
        registerStatsMetric(cat, core_id, (suffix + "_pages_prefetched").c_str(), &m_stats.depth_pages_prefetched[d]);
        registerStatsMetric(cat, core_id, (suffix + "_pages_failed").c_str(),     &m_stats.depth_pages_failed[d]);
    }
    registerStatsMetric(cat, core_id, "chained_predictions_total",           &m_stats.chained_predictions_total);
    registerStatsMetric(cat, core_id, "chained_prefetch_opportunities",      &m_stats.chained_prefetch_opportunities);
    registerStatsMetric(cat, core_id, "chained_prefetch_skipped_max_depth",  &m_stats.chained_prefetch_skipped_max_depth);

    // 16. Instruction vs Data breakdown
    registerStatsMetric(cat, core_id, "queries_instruction",                      &m_stats.queries_instruction);
    registerStatsMetric(cat, core_id, "queries_data",                              &m_stats.queries_data);
    registerStatsMetric(cat, core_id, "queries_with_payload_instruction",          &m_stats.queries_with_payload_instruction);
    registerStatsMetric(cat, core_id, "queries_with_payload_data",                 &m_stats.queries_with_payload_data);
    registerStatsMetric(cat, core_id, "predictions_issued_instruction",            &m_stats.predictions_issued_instruction);
    registerStatsMetric(cat, core_id, "predictions_issued_data",                   &m_stats.predictions_issued_data);
    registerStatsMetric(cat, core_id, "prefetch_successful_instruction",           &m_stats.prefetch_successful_instruction);
    registerStatsMetric(cat, core_id, "prefetch_successful_data",                  &m_stats.prefetch_successful_data);
    registerStatsMetric(cat, core_id, "prefetch_failed_instruction",               &m_stats.prefetch_failed_instruction);
    registerStatsMetric(cat, core_id, "prefetch_failed_data",                      &m_stats.prefetch_failed_data);
    registerStatsMetric(cat, core_id, "learning_opportunities_instruction",        &m_stats.learning_opportunities_instruction);
    registerStatsMetric(cat, core_id, "learning_opportunities_data",               &m_stats.learning_opportunities_data);
    registerStatsMetric(cat, core_id, "learning_updates_instruction",              &m_stats.learning_updates_instruction);
    registerStatsMetric(cat, core_id, "learning_updates_data",                     &m_stats.learning_updates_data);
    registerStatsMetric(cat, core_id, "total_prefetch_walk_latency_fs_instruction",&m_stats.total_prefetch_walk_latency_fs_instruction);
    registerStatsMetric(cat, core_id, "total_prefetch_walk_latency_fs_data",       &m_stats.total_prefetch_walk_latency_fs_data);

    // 17. Stride
    registerStatsMetric(cat, core_id, "stride_detected",                          &m_stats.stride_detected);
    registerStatsMetric(cat, core_id, "stride_changed",                           &m_stats.stride_changed);
    registerStatsMetric(cat, core_id, "stride_reset",                             &m_stats.stride_reset);

    // 18. Stride direct prefetch
    registerStatsMetric(cat, core_id, "stride_direct_issued",                     &m_stats.stride_direct_issued);
    registerStatsMetric(cat, core_id, "stride_direct_successful",                 &m_stats.stride_direct_successful);
    registerStatsMetric(cat, core_id, "stride_direct_failed",                     &m_stats.stride_direct_failed);

    // 19. 2MB huge-page plane
    registerStatsMetric(cat, core_id, "queries_2mb",                             &m_stats.queries_2mb);
    registerStatsMetric(cat, core_id, "predictions_2mb",                         &m_stats.predictions_2mb);
    registerStatsMetric(cat, core_id, "prefetch_successful_2mb",                 &m_stats.prefetch_successful_2mb);
    registerStatsMetric(cat, core_id, "learning_transitions_2mb",                &m_stats.learning_transitions_2mb);
    registerStatsMetric(cat, core_id, "learning_skipped_mixed_pagesize",         &m_stats.learning_skipped_mixed_pagesize);

    // OS-managed side-table access latency (payload_in_side_table)
    registerStatsMetric(cat, core_id, "side_table_accesses",                     &m_stats.side_table_accesses);
    registerStatsMetric(cat, core_id, "side_table_total_latency_fs",             &m_stats.side_table_total_latency_fs);
    registerStatsMetric(cat, core_id, "side_table_min_latency_fs",               &m_stats.side_table_min_latency_fs);
    registerStatsMetric(cat, core_id, "side_table_max_latency_fs",               &m_stats.side_table_max_latency_fs);
    registerStatsMetric(cat, core_id, "side_table_l1d",                          &m_stats.side_table_l1d);
    registerStatsMetric(cat, core_id, "side_table_l2",                           &m_stats.side_table_l2);
    registerStatsMetric(cat, core_id, "side_table_nuca",                         &m_stats.side_table_nuca);
    registerStatsMetric(cat, core_id, "side_table_dram",                         &m_stats.side_table_dram);
    registerStatsMetric(cat, core_id, "side_table_pwc_hits",                     &m_stats.side_table_pwc_hits);
    registerStatsMetric(cat, core_id, "side_table_pwc_misses",                   &m_stats.side_table_pwc_misses);
    registerStatsMetric(cat, core_id, "side_table_levels_accessed",              &m_stats.side_table_levels_accessed);

    // --- 21. In-PTE payload writeback modeling ---
    registerStatsMetric(cat, core_id, "payload_writes",                          &m_stats.payload_writes);
    registerStatsMetric(cat, core_id, "payload_dirty_marks_hit",                 &m_stats.payload_dirty_marks_hit);
    registerStatsMetric(cat, core_id, "payload_dirty_marks_miss",                &m_stats.payload_dirty_marks_miss);
    registerStatsMetric(cat, core_id, "payload_dirty_marks_coalesced",           &m_stats.payload_dirty_marks_coalesced);
}

// ============================================================================
// Finalize unique-page counters (call periodically or at teardown)
// ============================================================================

void TemporalPTEPrefetcher::finalizeUniquePageStats()
{
#if ENABLE_TPTE_UNIQUE_VPN_TRACKING
    m_stats.unique_vpns_seen             = m_unique_vpns_seen.size();
    m_stats.unique_vpns_with_payload     = m_unique_vpns_with_payload.size();
    m_stats.unique_vpns_prefetched_for   = m_unique_vpns_prefetched_for.size();
    m_stats.unique_vpns_learned          = m_unique_vpns_learned.size();
    m_stats.unique_predicted_vpns        = m_unique_predicted_vpns.size();
#endif
}

// ============================================================================
// Construction
// ============================================================================
//
// All configuration knobs are passed from the factory (tlb_prefetcher_factory.h)
// which reads them from the simulator config file (e.g., mmu_temporal_pte.cfg).
//
// Initialization order:
//   1. Codec layout validation (assert payload fits in 128 bits)
//   2. PC table allocation (only in PC_COND_LEARN mode)
//   3. Confidence policy instantiation (factory method)
//   4. Radix-tree backing store root (only when virtualize_pc_table=true)
//   5. Stats zeroing and registration
//   6. Configuration logging
// ============================================================================

TemporalPTEPrefetcher::TemporalPTEPrefetcher(
        Core *_core,
        MemoryManagerBase *_memory_manager,
        ShmemPerfModel *_shmem_perf_model,
        String name,
        uint32_t num_offsets,
        uint32_t offset_bits,
        uint32_t conf_bits,
        uint32_t base_bit,
        uint32_t conf_threshold,
        uint32_t page_shift,
        uint32_t region_shift,
        TemporalPTEMode mode,
        TemporalPTEReplacement replacement,
        uint32_t conf_init,
        bool     enable_decay,
        uint64_t decay_period,
        uint32_t pc_tag_bits,
        uint32_t pc_table_size,
        bool     reserve_global_slot,
        uint32_t max_prefetch_depth,
        float    chain_edge_decay,
        float    chain_score_threshold,
        uint32_t chain_conf_threshold,
        bool     learn_on_hit,
        bool     prefetch_on_hit,
        uint32_t stride_conf_threshold,
        bool     stride_direct_prefetch,
        uint32_t stride_direct_degree,
        bool     virtualize_pc_table,
        const std::string& confidence_policy,
        uint32_t conf_bump_amount,
        uint32_t conf_decay_on_bump,
        uint32_t conf_decay_on_miss,
        bool     payload_in_side_table,
        uint64_t side_table_base_pa,
        uint32_t side_table_payload_bits,
        bool     side_table_radix,
        uint32_t side_table_levels,
        uint32_t side_table_bits_per_level,
        uint32_t side_table_pwc_entries,
        bool     model_payload_writeback,
        bool     prefetch_install_pq)
    : TLBPrefetcherBase(_core, _memory_manager, _shmem_perf_model, name),
      m_codec(PTEOffsetConfig{num_offsets, offset_bits, conf_bits, base_bit, /*signed_offset=*/true}),
      m_sim_log("TemporalPTE", _core->getId(), DEBUG_TEMPORAL_PTE_PREFETCHER),
      m_page_shift(page_shift),
      m_region_shift(region_shift),
      m_region_pages(1u << region_shift),
      m_conf_threshold(conf_threshold),
      m_mode(mode),
      m_replacement(replacement),
      m_conf_init(conf_init),
      m_enable_decay(enable_decay),
      m_decay_period(decay_period),
      m_pc_tag_bits(pc_tag_bits),
      m_reserve_global_slot(reserve_global_slot),
      m_current_is_pc_miss(false),
      m_current_is_instruction(false),
      m_learn_on_hit(learn_on_hit),
      m_prefetch_on_hit(prefetch_on_hit),
      m_stride_conf_threshold(stride_conf_threshold),
      m_stride_direct_prefetch(stride_direct_prefetch),
      m_stride_direct_degree(stride_direct_degree),
      m_max_prefetch_depth(max_prefetch_depth),
      m_chain_edge_decay(chain_edge_decay),
      m_chain_score_threshold(chain_score_threshold),
      m_chain_fanout_penalty(1.0f / std::sqrt(static_cast<float>(std::max(1u, 1u << region_shift)))),
      m_chain_conf_threshold(chain_conf_threshold),
      m_last_region_id(0),
      m_last_pc(0),
      m_update_counter(0),
      m_rng(static_cast<uint32_t>(_core->getId())),
      m_stride_last_delta(0),
      m_stride_confidence(0),
      m_stride_value(0),
      m_stride_has_prev(false),
      m_pc_table_mask(0),
      m_virtualize_pc_table(virtualize_pc_table),
      m_payload_in_side_table(payload_in_side_table),
      m_side_table_base_pa(side_table_base_pa),
      m_side_table_bytes(0),
      m_side_table_payload_bits(side_table_payload_bits),
      m_side_table_reserved(false),
      m_side_table_radix(side_table_radix),
      m_side_table_levels(side_table_levels < 1 ? 1 : side_table_levels),
      m_side_table_bits_per_level(side_table_bits_per_level < 1 ? 1 : side_table_bits_per_level),
      m_side_table_pwc_entries(side_table_pwc_entries),
      m_model_payload_writeback(model_payload_writeback),
      m_prefetch_install_pq(prefetch_install_pq)
{
    // --- Validate codec layout ---
    assert(m_codec.config().fitsInPayload() && "PTE offset layout does not fit in 128-bit payload");

    // OS side-table: default payload width to the codec's actual bit count so a
    // 64B line packs 512/payload_bits payloads.  Config knob (side_table_payload_bits)
    // overrides; clamp to [1, 512].
    if (m_side_table_payload_bits == 0)
        m_side_table_payload_bits = static_cast<uint32_t>(m_codec.config().totalBits());
    if (m_side_table_payload_bits == 0)   m_side_table_payload_bits = 80;
    if (m_side_table_payload_bits > 512)  m_side_table_payload_bits = 512;

    // --- PC table (only used in PC_COND_LEARN mode) ---
    if (mode == TemporalPTEMode::PC_COND_LEARN && pc_table_size > 0)
    {
        // Round up to power-of-2
        uint32_t sz = 1;
        while (sz < pc_table_size) sz <<= 1;
        m_pc_table.resize(sz);
        m_pc_table_mask = sz - 1;
        for (auto& e : m_pc_table) { e.valid = false; e.pc_tag = 0; e.last_vpn = 0;
                                       e.stride_last_delta = 0; e.stride_confidence = 0; e.stride_value = 0; }
    }

    // --- Confidence policy ---
    m_confidence_policy = ConfidencePolicy::create(confidence_policy, conf_bump_amount,
                                                    conf_decay_on_bump, conf_decay_on_miss);

    // --- Radix-tree backing store (only when virtualized) ---
    m_pc_radix_root = nullptr;
    m_pc_radix_entries_stored = 0;
    if (m_virtualize_pc_table)
    {
        m_pc_radix_root = pcRadixAllocFrame(false);  // root = internal level
    }

    // --- Inflight/installed maps (initially empty) ---
    // No pre-allocation needed; maps grow/shrink naturally.

    // --- Zero stats struct ---
    memset(&m_stats, 0, sizeof(m_stats));
    m_stats.min_prefetch_walk_latency_fs = UINT64_MAX;  // sentinel for min tracking
    m_stats.max_prefetch_walk_latency_fs = 0;
    m_stats.total_prefetch_walk_latency_fs = 0;

    // --- Register all stats ---
    registerAllStats(_core->getId());

    // --- Log configuration ---
    {
        std::ostringstream oss;
        oss << "INIT: num_offsets=" << num_offsets
            << " offset_bits=" << offset_bits
            << " conf_bits=" << conf_bits
            << " base_bit=" << base_bit
            << " conf_threshold=" << conf_threshold
            << " page_shift=" << page_shift
            << " region_shift=" << region_shift
            << " region_pages=" << m_region_pages
            << " mode=" << static_cast<int>(mode)
            << " replacement=" << static_cast<int>(replacement)
            << " conf_init=" << conf_init
            << " decay=" << (enable_decay ? "on" : "off")
            << " decay_period=" << decay_period
            << " pc_tag_bits=" << pc_tag_bits
            << " pc_table_size=" << pc_table_size
            << " reserve_global_slot=" << (reserve_global_slot ? "on" : "off")
            << " max_prefetch_depth=" << max_prefetch_depth
            << " chain_edge_decay=" << chain_edge_decay
            << " chain_score_threshold=" << chain_score_threshold
            << " chain_conf_threshold=" << chain_conf_threshold
            << " chain_fanout_penalty=" << m_chain_fanout_penalty
            << " learn_on_hit=" << (learn_on_hit ? "on" : "off")
            << " prefetch_on_hit=" << (prefetch_on_hit ? "on" : "off")
            << " stride_conf_thresh=" << stride_conf_threshold
            << " stride_direct_prefetch=" << (stride_direct_prefetch ? "on" : "off")
            << " stride_direct_degree=" << stride_direct_degree
            << " virtualize_pc_table=" << (virtualize_pc_table ? "on" : "off")
            << " confidence_policy=" << m_confidence_policy->name();
        m_sim_log.info(oss.str());
    }
}

TemporalPTEPrefetcher::~TemporalPTEPrefetcher()
{
    // Flush unique-page counts one last time
#if ENABLE_TPTE_UNIQUE_VPN_TRACKING
    finalizeUniquePageStats();
#endif

    // Finalize inflight/installed end-of-sim stats
    m_stats.inflight_at_end  = static_cast<UInt64>(m_inflight.size());
    m_stats.installed_at_end = static_cast<UInt64>(m_installed.size());

    // Free radix-tree backing store
    if (m_pc_radix_root)
        pcRadixCleanup(m_pc_radix_root, 0);
}

// ============================================================================
// Core prefetch logic — performPrefetch()
// ============================================================================
//
// This is the main entry point, called by the TLB/PQ on every translation
// access (both hits and misses — gating is handled internally).
//
// The function is structured in four sequential phases:
//
//   PHASE 0: Timeliness tracking (always runs)
//     Check if the current demand region was previously prefetched.
//     Classify as: inflight (late), installed (useful or evicted), or
//     not-prefetched (true miss).  This provides accuracy feedback.
//
//   PHASE 1: Payload read + prediction (gated: miss or PQ-hit-with-prefetch_on_hit)
//     Read the PTE payload for the demand region.  Decode delta/conf slots
//     and issue PTWTransparent walks for each predicted target region.
//     Chained prefetching (BFS) follows predicted payloads up to
//     max_prefetch_depth levels.
//
//   PHASE 2: Learning (gated: miss or PQ-hit-with-learn_on_hit)
//     Observe the region-to-region transition (prev → current) and write
//     the delta back into the source region's PTE payload.  Uses the
//     pluggable confidence policy for bump/decay.
//
//   PHASE 3: Stride detection (gated: stride_direct_prefetch enabled)
//     Track page-level deltas per-PC (or globally).  When a stride is
//     confirmed, issue direct PTWTransparent walks along the stride.
//
// Parameters:
//   address     — virtual address of the demand access
//   eip         — instruction PC that triggered the translation
//   lock/modeled/count — standard Sniper modeling flags
//   pt          — pointer to the PageTable for PTW and payload access
//   instruction — true if this is an instruction translation (iTLB)
//   tlb_hit     — true if the TLB contained this entry (no miss)
//   pq_hit      — true if the TLB entry came from the prefetch queue
//
// Returns: vector of prefetched translations (address, PPN, timestamp)
//          to be inserted into the prefetch queue by the caller.
// ============================================================================

// ---------------------------------------------------------------------------
// OS-managed side-table access model (alternative to PTE-embedded payload)
// ---------------------------------------------------------------------------
// Models a single cache-line read to the OS-managed payload-table entry for
// `region_id`, issued in PARALLEL with the page-table walk (i.e. starting at
// the current elapsed time).  accessCache() snapshots/restores the caller's
// clock and returns ONLY the access latency, so the translation's own timing
// is unaffected -- the returned latency is used purely to delay the prefetch
// issue.  The access goes through the normal L1D->L2->LLC->DRAM hierarchy, so
// hot table entries cache naturally, matching an OS-managed in-memory table.
//
// The table has a fixed SIDE_TABLE_BYTES footprint in DRAM and is hash-indexed
// (wraparound) so the modelled physical address stays bounded and within the
// table's own region.  The 2MB plane is mixed into a disjoint slot stream so
// it does not systematically alias the 4KB plane.
// Reserve the OS-managed payload table from the physical allocator, using the
// SAME mechanism the hash page tables (HT/Cuckoo/HDC) use to reserve their
// large contiguous regions: handle_page_table_allocations(bytes) carves the
// bytes out of the kernel-reserved area and returns the base PPN.  Idempotent.
void TemporalPTEPrefetcher::reserveSideTable()
{
    if (m_side_table_reserved)
        return;
    m_side_table_reserved = true;   // set first so we don't retry on failure

    auto* os = Sim()->getMimicOS();
    auto* alloc = os ? os->getMemoryAllocator() : nullptr;
    if (alloc != nullptr)
    {
        UInt64 base_ppn = alloc->handle_page_table_allocations(SIDE_TABLE_RESERVE_BYTES);
        m_side_table_base_pa = base_ppn * 4096ULL;
        m_side_table_bytes   = SIDE_TABLE_RESERVE_BYTES;
        std::ostringstream oss;
        oss << "OS payload side-table reserved: " << (SIDE_TABLE_RESERVE_BYTES >> 20)
            << " MB @ PA 0x" << std::hex << m_side_table_base_pa;
        m_sim_log.info(oss.str());
    }
    else if (m_side_table_bytes == 0)
    {
        // Fallback: keep the configured base, bound to 1 GB.
        m_side_table_bytes = SIDE_TABLE_RESERVE_BYTES;
    }
}

// Independent radix side-car walk (Flavor A): L levels, upper (pointer) levels
// filtered by the side-car PWC, leaf always fetched.  Each surviving level is a
// dependent accessCache chained from the current time (so the whole walk runs
// in parallel with the page-table walk).  Returns total walk latency; records
// PWC hits/misses, levels actually accessed, and the leaf hit-where.
SubsecondTime TemporalPTEPrefetcher::modelSideTableRadixWalk(
        uint64_t line_no, IntPtr eip, Core::lock_signal_t lock, bool modeled, bool count)
{
    MemoryManagementUnitBase *mmu = memory_manager->getMMU();
    if (mmu == nullptr) return SubsecondTime::Zero();

    const uint32_t L = m_side_table_levels;
    const uint32_t B = m_side_table_bits_per_level;
    const uint64_t per_level_bytes = std::max<uint64_t>(64ULL, m_side_table_bytes / L);
    const uint64_t per_level_lines = per_level_bytes / 64ULL;

    SubsecondTime acc = SubsecondTime::Zero();
    const SubsecondTime t0 = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

    for (uint32_t lvl = 0; lvl < L; ++lvl)
    {
        const bool is_leaf = (lvl + 1 == L);
        // Leaf is addressed by the full line_no (unique per leaf line); a pointer
        // level is addressed by the high-order prefix, so nearby line_nos share
        // upper nodes (→ PWC/cache locality, as in a real radix walk).
        const uint64_t shift = static_cast<uint64_t>(L - 1 - lvl) * B;
        const uint64_t node_id = is_leaf ? line_no : (shift >= 64 ? 0ULL : (line_no >> shift));
        const uint64_t line_in_level = per_level_lines ? (node_id % per_level_lines) : 0ULL;
        const IntPtr node_pa = static_cast<IntPtr>(
            m_side_table_base_pa + static_cast<uint64_t>(lvl) * per_level_bytes + line_in_level * 64ULL);

        // Upper (pointer) levels are filtered by the side-car page-walk cache.
        if (!is_leaf)
        {
            if (m_sidecar_pwc_set.count(node_pa))
            {
                m_stats.side_table_pwc_hits++;
                m_sidecar_pwc.remove(node_pa);
                m_sidecar_pwc.push_front(node_pa);   // MRU
                continue;                            // PWC hit → no memory access
            }
            m_stats.side_table_pwc_misses++;
            m_sidecar_pwc.push_front(node_pa);
            m_sidecar_pwc_set.insert(node_pa);
            while (m_side_table_pwc_entries > 0 && m_sidecar_pwc.size() > m_side_table_pwc_entries)
            {
                m_sidecar_pwc_set.erase(m_sidecar_pwc.back());
                m_sidecar_pwc.pop_back();
            }
        }

        // Dependent memory access for this level (starts after the prior ones).
        MemoryManagementUnitBase::translationPacket pkt(
            node_pa, eip, /*instruction=*/false, lock, modeled, count,
            CacheBlockInfo::block_type_t::PAGE_TABLE_DATA);
        HitWhere::where_t hw = HitWhere::UNKNOWN;
        acc += mmu->accessCache(pkt, t0 + acc, /*is_prefetch=*/false, hw);
        m_stats.side_table_levels_accessed++;

        if (is_leaf) {
            switch (hw) {
                case HitWhere::L1_OWN:     m_stats.side_table_l1d++;  break;
                case HitWhere::L2_OWN:     m_stats.side_table_l2++;   break;
                case HitWhere::NUCA_CACHE: m_stats.side_table_nuca++; break;
                case HitWhere::DRAM_LOCAL:
                case HitWhere::DRAM_REMOTE:
                case HitWhere::DRAM:       m_stats.side_table_dram++; break;
                default: break;
            }
        }
    }
    return acc;
}

SubsecondTime TemporalPTEPrefetcher::modelSideTableAccess(
        uint64_t region_id, IntPtr eip, Core::lock_signal_t lock, bool modeled, bool count)
{
    MemoryManagementUnitBase *mmu = memory_manager->getMMU();
    if (mmu == nullptr || m_side_table_payload_bits == 0)
        return SubsecondTime::Zero();

    reserveSideTable();   // lazily reserve the 1 GB region on first use

    const bool is_2mb = (region_id & REGION_2MB_TAG) != 0;
    uint64_t idx = region_id & ~REGION_2MB_TAG;
    if (is_2mb)
        idx ^= 0x9E3779B97F4A7C15ULL;   // golden-ratio mix to separate planes

    // A 64B (512-bit) line packs floor(512/payload_bits) payloads; `line_no` is
    // the leaf line holding this region's payload (adjacent regions share it).
    const uint32_t per_line = std::max(1u, 512u / m_side_table_payload_bits);
    const uint64_t line_no = idx / per_line;

    SubsecondTime lat;
    if (m_side_table_radix)
    {
        lat = modelSideTableRadixWalk(line_no, eip, lock, modeled, count);
    }
    else
    {
        // Flat organization: single direct-mapped 64B line read.
        const uint64_t line_off = (line_no * 64ULL) % m_side_table_bytes;
        const IntPtr entry_pa = static_cast<IntPtr>(m_side_table_base_pa + line_off);
        MemoryManagementUnitBase::translationPacket pkt(
            entry_pa, eip, /*instruction=*/false, lock, modeled, count,
            CacheBlockInfo::block_type_t::DATA);
        HitWhere::where_t hw = HitWhere::UNKNOWN;
        const SubsecondTime t_now = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);
        lat = mmu->accessCache(pkt, t_now, /*is_prefetch=*/false, hw);
        switch (hw) {
            case HitWhere::L1_OWN:     m_stats.side_table_l1d++;  break;
            case HitWhere::L2_OWN:     m_stats.side_table_l2++;   break;
            case HitWhere::NUCA_CACHE: m_stats.side_table_nuca++; break;
            case HitWhere::DRAM_LOCAL:
            case HitWhere::DRAM_REMOTE:
            case HitWhere::DRAM:       m_stats.side_table_dram++; break;
            default: break;
        }
    }

    // --- Common latency instrumentation (total walk latency per access) ---
    const UInt64 lat_fs = lat.getFS();
    m_stats.side_table_accesses++;
    m_stats.side_table_total_latency_fs += lat_fs;
    if (m_stats.side_table_accesses == 1 || lat_fs < m_stats.side_table_min_latency_fs)
        m_stats.side_table_min_latency_fs = lat_fs;
    if (lat_fs > m_stats.side_table_max_latency_fs)
        m_stats.side_table_max_latency_fs = lat_fs;
    return lat;
}

std::vector<query_entry> TemporalPTEPrefetcher::performPrefetch(
        IntPtr address, IntPtr eip, Core::lock_signal_t lock,
        bool modeled, bool count, PageTable *pt, bool instruction, bool tlb_hit, bool pq_hit, int page_size)
{
    std::vector<query_entry> result;

    if (pt == nullptr)
        return result;

    // Safety: drop any payload-writeback entries not consumed by a prior flush
    // (e.g. if the previous access's walk faulted), so we never dirty a stale PTE.
    if (m_model_payload_writeback)
        m_pending_payload_wb.clear();

    // Always discover the real page size via a lightweight PT lookup (no cache
    // modelling).  The TLB-reported page_size can be wrong — a 4KB-only L1 dTLB
    // reports page_size=12 even when the underlying mapping is 2MB.
    bool page_mapped = false;
    {
        PTWResult probe = pt->initializeWalk(address, /*count*/ false, /*is_prefetch*/ false, /*restart*/ false);
        if (!probe.fault_happened && probe.page_size > 0)
        {
            page_size = probe.page_size;
            page_mapped = true;
        }
    }

    // If the page is not yet mapped (first-touch before fault handling),
    // skip everything — there's no payload to read and the transition
    // would pollute the learning tables with noise from cold faults.
    if (!page_mapped)
        return result;

    const bool is_2mb = (page_size == 21);

    // Compute VPN and region_id in the correct plane
    const uint64_t vpn = static_cast<uint64_t>(address) >> m_page_shift;          // always 4KB VPN
    const uint64_t vpn_2mb = static_cast<uint64_t>(address) >> PAGE_SHIFT_2MB;    // 2MB VPN
    const uint64_t region_id = is_2mb
        ? ((vpn_2mb >> m_region_shift) | REGION_2MB_TAG)   // 2MB region with tag
        : (vpn >> m_region_shift);                          // 4KB region (as before)

    // ---- Stat: invocation tracking ----
    m_stats.queries++;
    m_stats.demand_accesses++;
    if (is_2mb)
        m_stats.queries_2mb++;
    m_current_is_instruction = instruction;
    if (instruction)
        m_stats.queries_instruction++;
    else
        m_stats.queries_data++;
#if ENABLE_TPTE_UNIQUE_VPN_TRACKING
    m_unique_vpns_seen.insert(vpn);
#endif

    // ---- PHASE 0: Timeliness tracking (region granularity) ----
    //
    // Two maps track the lifecycle of every prefetched region:
    //   m_inflight  — region predicted, PTW issued, but not yet installed in TLB
    //   m_installed — PTW completed and TLB entry materialized (via notifyInstall)
    //
    // On each demand access we check:
    //   (a) region in m_inflight  → LATE: demand arrived before walk finished
    //   (b) region in m_installed → ACCURATE: prefetch beat demand
    //       - with pq_hit  → USEFUL: TLB still held the page (demand was served)
    //       - without pq_hit → EVICTED: TLB evicted it before demand (wasted)
    //   (c) neither → TRUE MISS: region was never prefetched
    //
    // After classification the entry is consumed (erased) so each region
    // is counted exactly once.  Remaining entries at end-of-sim are unused.
    {
        uint64_t now_ns = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD).getNS();

        auto it_inflight = m_inflight.find(region_id);
        if (it_inflight != m_inflight.end())
        {
            // LATE: demand arrived while walk is still pending (not yet in TLB)
            const InflightEntry& entry = it_inflight->second;
            m_stats.demand_hit_inflight++;

            // Per-source
            switch (entry.source) {
                case PrefetchSource::TEMPORAL: m_stats.demand_hit_inflight_temporal++; break;
                case PrefetchSource::STRIDE:   m_stats.demand_hit_inflight_stride++;   break;
                case PrefetchSource::CHAINED:  m_stats.demand_hit_inflight_chained++;  break;
            }

            // Per-depth
            if (entry.depth < Stats::MAX_CHAIN_DEPTH)
                m_stats.depth_demand_hit_inflight[entry.depth]++;

            // Late gap: how much walk time remains (completion_time - demand_time)
            if (entry.completion_time_ns > now_ns)
            {
                uint64_t gap = entry.completion_time_ns - now_ns;
                m_stats.sum_inflight_remaining_ns += gap;
                m_stats.sum_walk_latency_saved_inflight_ns += gap;
                if (gap > m_stats.max_inflight_remaining_ns)
                    m_stats.max_inflight_remaining_ns = gap;

                // Timeliness histogram (remaining time)
                uint32_t bucket;
                if      (gap < 1000)    bucket = 0;  // <1us
                else if (gap < 10000)   bucket = 1;  // 1-10us
                else if (gap < 100000)  bucket = 2;  // 10-100us
                else if (gap < 1000000) bucket = 3;  // 100us-1ms
                else                    bucket = 4;  // >=1ms
                m_stats.inflight_remaining_bucket[bucket]++;
            }

            m_inflight.erase(it_inflight);
        }
        else
        {
            auto it_installed = m_installed.find(region_id);
            if (it_installed != m_installed.end())
            {
                // INSTALLED: prefetch was installed in TLB before demand
                const InflightEntry& entry = it_installed->second;

                if (pq_hit)
                    m_stats.demand_hit_installed++;      // TLB still held the page
                else
                    m_stats.demand_hit_installed_evicted++; // TLB evicted before demand

                // Per-source (count both installed + evicted together)
                switch (entry.source) {
                    case PrefetchSource::TEMPORAL: m_stats.demand_hit_installed_temporal++; break;
                    case PrefetchSource::STRIDE:   m_stats.demand_hit_installed_stride++;   break;
                    case PrefetchSource::CHAINED:  m_stats.demand_hit_installed_chained++;  break;
                }

                // Per-depth
                if (entry.depth < Stats::MAX_CHAIN_DEPTH)
                    m_stats.depth_demand_hit_installed[entry.depth]++;

                // Lead time: demand_time - completion_time (positive = early)
                if (now_ns > entry.completion_time_ns)
                {
                    uint64_t lead = now_ns - entry.completion_time_ns;
                    m_stats.sum_completion_to_demand_ns += lead;
                    if (lead > m_stats.max_completion_to_demand_ns)
                        m_stats.max_completion_to_demand_ns = lead;

                    // Timeliness histogram (lead time for installed hits)
                    uint32_t bucket;
                    if      (lead < 1000)    bucket = 0;  // <1us
                    else if (lead < 10000)   bucket = 1;  // 1-10us
                    else if (lead < 100000)  bucket = 2;  // 10-100us
                    else if (lead < 1000000) bucket = 3;  // 100us-1ms
                    else                     bucket = 4;  // >=1ms
                    m_stats.installed_lead_time_bucket[bucket]++;
                }

                // Latency saved: for installed hits the full walk was avoided
                if (pq_hit)
                    m_stats.sum_walk_latency_saved_installed_ns += entry.walk_latency_ns;

                m_installed.erase(it_installed);
            }
            else
            {
                // TRUE MISS: region was never prefetched (or was already consumed)
                m_stats.demand_miss_not_prefetched++;
            }
        }
    }
    // ----------------------------------------------------------------
    // 1. Read leaf PTE payload for the first page of current region
    //    (payloads are stored per-region, anchored at the base VPN)
    //    Gating policy:
    //      - TLB miss          → always prefetch
    //      - Regular TLB hit   → never prefetch (entry wasn't from PQ)
    //      - PQ hit            → prefetch only if m_prefetch_on_hit
    // ----------------------------------------------------------------
    if (!tlb_hit || (pq_hit && m_prefetch_on_hit))
    {
    // Read payload from the correct level (PTE for 4KB, PMD for 2MB)
    uint64_t region_base_vpn;
    PayloadWord pte_payload;
    if (is_2mb)
    {
        uint64_t region_id_raw = vpn_2mb >> m_region_shift;  // without tag
        region_base_vpn = region_id_raw << m_region_shift;   // base 2MB VPN of region
        pte_payload = pt->readPMDPayloadBits(region_base_vpn);
    }
    else
    {
        region_base_vpn = (vpn >> m_region_shift) << m_region_shift;
        pte_payload = pt->readPayloadBits(region_base_vpn);
    }

    if (pte_payload != 0)
    {
        m_stats.queries_with_payload++;
        if (instruction)
            m_stats.queries_with_payload_instruction++;
        else
            m_stats.queries_with_payload_data++;
#if ENABLE_TPTE_UNIQUE_VPN_TRACKING
        m_unique_vpns_with_payload.insert(vpn);
#endif
    }
    else
    {
        m_stats.queries_without_payload++;
    }

    // --- Debug: log incoming access and payload ---
    if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG))
    {
        std::ostringstream oss;
        oss << "QUERY #" << m_stats.queries
            << " addr=0x" << std::hex << address
            << " vpn=0x" << vpn
            << " region=0x" << region_id
            << " base_vpn=0x" << region_base_vpn
            << " eip=0x" << eip << std::dec
            << " payload=" << m_codec.dumpPayload(pte_payload);
        m_sim_log.debug(oss.str());
    }

    // ----------------------------------------------------------------
    // 2. Decode entries and generate predictions (region-level)
    //    Each delta is in units of regions (region_pages pages each).
    //    For each predicted target region, we prefetch ALL pages in it.
    //    
    //    Chained prefetching: when max_prefetch_depth > 0, we use a work
    //    queue to follow prefetched payloads up to the configured depth.
    // ----------------------------------------------------------------
    
    // ---- PHASE 1: BFS chained prediction engine ----
    //
    // Work queue implements breadth-first traversal of the prediction graph:
    //
    //   Depth 0: demand region's payload → decode slots → predict regions
    //   Depth 1: each predicted region's payload → decode → predict deeper
    //   Depth 2: ... (up to max_prefetch_depth)
    //
    // BFS ensures nearer predictions (more likely to be demanded soon)
    // get earlier start times.  Each work item carries:
    //   - region_id: the source region whose payload we decode
    //   - payload: the 128-bit PTE payload to decode
    //   - depth: current chain depth (0 = demand-triggered)
    //   - chain_start_time: earliest time this walk can begin (serialized
    //     after parent's PTW completes)
    //   - path_score: multiplicative confidence along the chain, used for
    //     gating at depth > 0 (depth 0 uses simple integer threshold)
    //
    // Cycle avoidance: visited_regions set prevents re-predicting a region
    // that was already predicted at a shallower depth.
    struct ChainWorkItem {
        uint64_t region_id;
        PayloadWord payload;
        uint32_t depth;
        SubsecondTime chain_start_time;  // Earliest time this chained walk can begin
        float path_score;                // Accumulated confidence along the chain (1.0 at root)
    };
    std::vector<ChainWorkItem> work_queue;
    std::unordered_set<uint64_t> visited_regions;  // Avoid cycles

    // Save the original shmem time so we can restore it after all prefetch walks
    const SubsecondTime original_shmem_time = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

    // Start with depth 0 (demand-triggered) — begins at current time, full confidence
    work_queue.push_back({region_id, pte_payload, 0, original_shmem_time, 1.0f});
    visited_regions.insert(region_id);

    // Local counters for this call (avoid using cumulative stats for gating)
    uint32_t regions_issued_this_call = 0;
    uint32_t chained_issued_this_call = 0;

    // FIFO (BFS) traversal: process breadth-first so nearer predictions
    // (more likely to be demanded soon) get the earliest start times.
    size_t work_idx = 0;
    while (work_idx < work_queue.size())
    {
        ChainWorkItem work = work_queue[work_idx++];
        
        uint64_t work_region_id = work.region_id;
        PayloadWord work_payload = work.payload;
        uint32_t current_depth = work.depth;
        
        // Skip if no payload to decode
        if (work_payload == 0)
            continue;
        
        auto entries = m_codec.decode(work_payload);
        
        // ---- Stat: slot-level analysis (only for depth 0) ----
        if (current_depth == 0)
        {
            for (uint32_t i = 0; i < entries.size(); i++)
            {
                m_stats.total_slots_decoded++;
                m_stats.sum_conf_all_slots += entries[i].conf;
                if (entries[i].conf > 0)  m_stats.slots_with_nonzero_conf++;
                if (entries[i].delta_vpn != 0) m_stats.slots_with_nonzero_delta++;
            }
        }

        // Sort by confidence (descending)
        struct SortEntry {
            uint32_t slot_idx;
            uint32_t conf;
            const PTEOffsetEntry *entry;
        };
        std::vector<SortEntry> sorted;
        sorted.reserve(entries.size());
        for (uint32_t i = 0; i < entries.size(); i++)
            sorted.push_back({i, entries[i].conf, &entries[i]});
        std::sort(sorted.begin(), sorted.end(),
                  [](const SortEntry& a, const SortEntry& b) { return a.conf > b.conf; });

        // --- Trace: log decoded slots ---
        if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
        {
            for (uint32_t i = 0; i < entries.size(); i++)
            {
                std::ostringstream oss;
                oss << "  [D" << current_depth << "] SLOT[" << i << "] delta=" << entries[i].delta_vpn
                    << " conf=" << entries[i].conf;
                m_sim_log.trace(oss.str());
            }
        }

        uint32_t regions_issued_this_level = 0;

        for (const auto& se : sorted)
        {
            if (se.entry->delta_vpn == 0)
            {
                if (current_depth == 0) m_stats.predictions_skipped_zero_delta++;
                if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
                    m_sim_log.trace("  SKIP slot=", se.slot_idx, "reason=zero_delta");
                continue;
            }

            // ---- Confidence gating ----
            //
            // Two models depending on depth:
            //
            //   Depth 0 (demand-triggered):
            //     Simple integer threshold: conf >= conf_threshold.
            //     Fast, no floating-point, appropriate for the latency-critical
            //     demand path.  With 2-bit confidence and conf_threshold=2,
            //     a slot needs 2 confirming observations before it fires.
            //
            //   Depth > 0 (chained):
            //     Multiplicative path-score model:
            //       child_score = parent_score × (local_conf / max_conf)
            //                     × edge_decay × fanout_penalty
            //
            //     - parent_score: accumulated confidence from root (1.0) down
            //     - local_conf / max_conf: normalized slot confidence (0..1)
            //     - edge_decay: per-hop decay (e.g., 0.85), models uncertainty growth
            //     - fanout_penalty: 1/sqrt(region_pages), penalizes wider regions
            //       because each fan-out dilutes per-page accuracy
            //
            //     The child is issued only if child_score >= chain_score_threshold.
            //     This naturally suppresses deep chains behind weak parents and
            //     prevents exponential prefetch explosion.
            //
            //   Example with region_shift=3 (fanout=1/sqrt(8)=0.354), edge_decay=0.85:
            //     Depth 0→1: 1.0 × 1.0 × 0.85 × 0.354 = 0.30 (passes 0.05 threshold)
            //     Depth 1→2: 0.30 × 1.0 × 0.85 × 0.354 = 0.09 (passes)
            //     Depth 2→3: 0.09 × 1.0 × 0.85 × 0.354 = 0.027 (fails → pruned)
            float child_score = 0.0f;
            if (current_depth == 0)
            {
                // Depth 0: simple integer threshold (unchanged from original)
                if (se.entry->conf < m_conf_threshold)
                {
                    m_stats.predictions_skipped_low_conf++;
                    if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
                        m_sim_log.trace("  SKIP slot=", se.slot_idx, "reason=low_conf conf=", se.entry->conf,
                                        " threshold=", m_conf_threshold);
                    continue;
                }
                // Compute score for potential chaining from this depth-0 prediction
                float local_conf = static_cast<float>(se.entry->conf) / static_cast<float>(std::max(1u, m_codec.config().maxConf()));
                child_score = work.path_score * local_conf * m_chain_edge_decay * m_chain_fanout_penalty;
            }
            else
            {
                // Depth > 0: optional integer confidence floor + path-score gating
                if (m_chain_conf_threshold > 0 && se.entry->conf < m_chain_conf_threshold)
                {
                    if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
                        m_sim_log.trace("  SKIP slot=", se.slot_idx, "reason=chain_low_conf conf=", se.entry->conf,
                                        " threshold=", m_chain_conf_threshold, " depth=", current_depth);
                    continue;
                }
                float local_conf = static_cast<float>(se.entry->conf) / static_cast<float>(std::max(1u, m_codec.config().maxConf()));
                child_score = work.path_score * local_conf * m_chain_edge_decay * m_chain_fanout_penalty;
                if (child_score < m_chain_score_threshold)
                {
                    if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
                        m_sim_log.trace("  SKIP slot=", se.slot_idx, "reason=low_path_score score=", child_score,
                                        " threshold=", m_chain_score_threshold, " depth=", current_depth);
                    continue;
                }
            }

            // No cap — confidence is the sole gatekeeper at all depths.

            // Predict target region
            // Strip REGION_2MB_TAG before delta arithmetic, re-apply after
            bool work_is_2mb = (work_region_id & REGION_2MB_TAG) != 0;
            uint64_t work_region_raw = work_region_id & ~REGION_2MB_TAG;
            int64_t predicted_region = static_cast<int64_t>(work_region_raw) + se.entry->delta_vpn;
            if (predicted_region <= 0)
            {
                if (current_depth == 0) m_stats.predictions_skipped_negative_vpn++;
                if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
                    m_sim_log.trace("  SKIP slot=", se.slot_idx, "reason=negative_region predicted=", predicted_region);
                continue;
            }

            // Re-apply the 2MB tag so downstream code knows the plane
            uint64_t target_region_u64 = static_cast<uint64_t>(predicted_region) | (work_is_2mb ? REGION_2MB_TAG : 0);
            if (visited_regions.count(target_region_u64) > 0)
            {
                if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
                    m_sim_log.trace("  SKIP slot=", se.slot_idx, "reason=already_visited region=0x", SimLog::hex(target_region_u64));
                continue;
            }
            visited_regions.insert(target_region_u64);

            // Fan out: prefetch ALL pages in the predicted region.
            // For 2MB plane, the target_region_u64 has the REGION_2MB_TAG set — strip it.
            bool target_is_2mb = (target_region_u64 & REGION_2MB_TAG) != 0;
            uint64_t target_region_raw = target_region_u64 & ~REGION_2MB_TAG;
            uint64_t base_vpn_of_region = target_region_raw << m_region_shift;
            bool any_page_succeeded = false;
            uint32_t pages_succeeded_this_region = 0;
            PayloadWord chained_payload = 0;  // Collect payload for chained prefetching

            // TLB residency check: if >50% of region pages already in any TLB, skip this region
            uint32_t fan_page_shift = target_is_2mb ? PAGE_SHIFT_2MB : m_page_shift;
            if (!m_tlb_hierarchy.empty())
            {
                uint32_t pages_in_tlb = 0;
                for (uint32_t p = 0; p < m_region_pages; p++)
                {
                    IntPtr page_addr = static_cast<IntPtr>(base_vpn_of_region + p) << fan_page_shift;
                    for (auto *tlb : m_tlb_hierarchy)
                    {
                        if (tlb->contains(page_addr, fan_page_shift))
                        {
                            pages_in_tlb++;
                            break;  // Found in at least one TLB, no need to check others
                        }
                    }
                }
                if (pages_in_tlb * 2 > m_region_pages)  // >50% threshold
                {
                    m_stats.predictions_skipped_tlb_residency++;
                    if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
                        m_sim_log.trace("  SKIP region=0x", SimLog::hex(target_region_u64),
                                        " reason=tlb_residency pages_in_tlb=", pages_in_tlb,
                                        "/", m_region_pages);
                    continue;
                }
            }

            if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG))
            {
                std::ostringstream oss;
                oss << "  [D" << current_depth << "] PREDICT slot=" << se.slot_idx
                    << " delta=" << se.entry->delta_vpn
                    << " conf=" << se.entry->conf
                    << " target_region=0x" << std::hex << predicted_region
                    << " base_vpn=0x" << base_vpn_of_region << std::dec
                    << " fanning_out " << m_region_pages << " pages";
                m_sim_log.debug(oss.str());
            }

            // Record timestamp before the walk batch for latency measurement.
            // For chained walks (depth > 0), advance shmem time to the parent's
            // completion time so the PTW correctly starts after the prior walk arrives.
            if (current_depth > 0)
                shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, work.chain_start_time);
            SubsecondTime t_before = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

            // === Multi-cache-line walk optimization ===
            //
            // Physical PTE layout in memory:
            //   - Each PTE is 8 bytes
            //   - A 64-byte cache line holds 8 consecutive PTEs
            //   - PTEs for consecutive VPNs are contiguous in the last-level
            //     page table frame
            //
            // Optimization: for each cache-line-aligned group of 8 pages
            // within the predicted region, we issue ONE full modeled walk
            // (PTWTransparent) for the first page ("cache-line leader").
            // This walk traverses the full page table, fetching the PTE
            // cache line into L1D/L2/LLC.  The remaining 7 sibling pages
            // within the same cache line reuse the leader's timestamp
            // (they only do a lightweight pt->initializeWalk to get the PPN
            // — no cache hierarchy access needed since the data is already
            // in the cache from the leader walk).
            //
            // Cost per region (modeled walks):
            //   region_shift=3 → 8 pages / 8 PTEs/CL = 1 walk
            //   region_shift=4 → 16 pages / 8 PTEs/CL = 2 walks (serialized)
            //   region_shift=5 → 32 pages / 8 PTEs/CL = 4 walks (serialized)
            //
            // This is why larger region_shift values have higher walk latency:
            // each additional cache line requires a full PTW traversal.
            static constexpr uint32_t PTES_PER_CACHE_LINE = 8;  // 64B / 8B

            // Track the latest completion across all cache-line walks for chaining
            SubsecondTime region_completion = SubsecondTime::Zero();

            // For 2MB: each "page" in the region is a 2MB page. PMD entries are 8B each,
            // so 8 consecutive PMD entries fit in one 64B cache line — same CL optimization.
            for (uint32_t p = 0; p < m_region_pages; p++)
            {
                uint64_t target_vpn = base_vpn_of_region + p;
                IntPtr target_addr = static_cast<IntPtr>(target_vpn) << fan_page_shift;

                query_entry q;
                bool is_cache_line_leader = (p % PTES_PER_CACHE_LINE == 0);

                if (is_cache_line_leader)
                {
                    // First page of a new PTE cache line — full modeled walk
                    m_stats.prefetch_attempts++;
                    q = PTWTransparent(target_addr, eip, lock, modeled, count, pt);
                    region_completion = q.timestamp;
                }
                else
                {
                    // Sibling page within the same PTE cache line — lightweight lookup
                    m_stats.prefetch_attempts++;
                    PTWResult sibling_result = pt->initializeWalk(target_addr, /*count*/ false, /*is_prefetch*/ true, /*restart_walk*/ true);
                    q.address = target_addr;
                    q.ppn = sibling_result.ppn;
                    q.page_size = sibling_result.page_size;
                    q.timestamp = region_completion;  // Same cache line → same latency
                    q.payload_bits = 0;  // Payload is per-region (at base VPN), not per-page
                }

                if (q.ppn != 0)
                {
                    result.push_back(q);
                    m_stats.prefetch_successful++;
                    if (target_is_2mb)
                        m_stats.prefetch_successful_2mb++;
                    m_stats.region_pages_prefetched++;
                    any_page_succeeded = true;
                    pages_succeeded_this_region++;
                    if (instruction)
                        m_stats.prefetch_successful_instruction++;
                    else
                        m_stats.prefetch_successful_data++;
                    
                    // Capture full 128-bit payload for chaining (read directly from
                    // page table to avoid truncation through 64-bit query_entry::payload_bits)
                    if (p == 0)
                        chained_payload = target_is_2mb
                            ? pt->readPMDPayloadBits(base_vpn_of_region)
                            : pt->readPayloadBits(base_vpn_of_region);

                    // ---- Stat: walk latency (femtoseconds) ----
                    // Only count for cache-line leaders (actual modeled walks).
                    // Sibling pages share the same cache line and timestamp;
                    // counting them would inflate the total by PTES_PER_CACHE_LINE.
                    if (is_cache_line_leader)
                    {
                        SubsecondTime walk_latency_ss = (q.timestamp > t_before)
                                                        ? (q.timestamp - t_before)
                                                        : SubsecondTime::Zero();
                        UInt64 walk_fs = walk_latency_ss.getFS();
                        m_stats.total_prefetch_walk_latency_fs += walk_fs;
                        if (instruction)
                            m_stats.total_prefetch_walk_latency_fs_instruction += walk_fs;
                        else
                            m_stats.total_prefetch_walk_latency_fs_data += walk_fs;
                        if (walk_fs > 0 && walk_fs < m_stats.min_prefetch_walk_latency_fs)
                            m_stats.min_prefetch_walk_latency_fs = walk_fs;
                        if (walk_fs > m_stats.max_prefetch_walk_latency_fs)
                            m_stats.max_prefetch_walk_latency_fs = walk_fs;
                    }

#if ENABLE_TPTE_UNIQUE_VPN_TRACKING
                m_unique_predicted_vpns.insert(target_vpn);
#endif
                
                if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
                {
                    std::ostringstream oss;
                    oss << "    PAGE[" << p << "] vpn=0x" << std::hex << target_vpn
                        << " -> ppn=0x" << q.ppn << std::dec
                        << (is_cache_line_leader ? " leader" : " sibling") << " OK";
                    m_sim_log.trace(oss.str());
                }
            }
            else
            {
                m_stats.prefetch_failed++;
                m_stats.region_pages_failed++;
                if (instruction)
                    m_stats.prefetch_failed_instruction++;
                else
                    m_stats.prefetch_failed_data++;
                
                // ---- Stat: per-depth tracking ----
                if (current_depth < Stats::MAX_CHAIN_DEPTH)
                    m_stats.depth_pages_failed[current_depth]++;

                if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
                {
                    std::ostringstream oss;
                    oss << "    PAGE[" << p << "] vpn=0x" << std::hex << target_vpn
                        << std::dec << " FAILED (no mapping)";
                    m_sim_log.trace(oss.str());
                }
            }
        }

        if (any_page_succeeded)
        {
            // ---- Inflight tracking: insert once per predicted region ----
            {
                PrefetchSource src = (current_depth == 0) ? PrefetchSource::TEMPORAL
                                                          : PrefetchSource::CHAINED;
                insertInflight(target_region_u64, t_before.getNS(), region_completion.getNS(),
                              src, se.entry->conf, current_depth);
            }

            m_stats.predictions_issued++;
            m_stats.region_predictions_issued++;
            if (target_is_2mb)
                m_stats.predictions_2mb++;
            regions_issued_this_level++;
            regions_issued_this_call++;
            if (instruction)
                m_stats.predictions_issued_instruction++;
            else
                m_stats.predictions_issued_data++;

            // ---- Stat: per-depth tracking ----
            if (current_depth < Stats::MAX_CHAIN_DEPTH)
            {
                m_stats.depth_predictions[current_depth]++;
                m_stats.depth_pages_prefetched[current_depth] += pages_succeeded_this_region;
            }
            if (current_depth > 0)
            {
                m_stats.chained_predictions_total++;
                chained_issued_this_call++;
            }

            // ---- Stat: confidence at prediction ----
            m_stats.sum_conf_at_prediction += se.entry->conf;
            if (se.entry->conf > m_stats.max_conf_at_prediction)
                m_stats.max_conf_at_prediction = se.entry->conf;

            // ---- Stat: delta magnitude ----
            uint64_t abs_d = static_cast<uint64_t>(
                se.entry->delta_vpn >= 0 ? se.entry->delta_vpn : -se.entry->delta_vpn);
            m_stats.sum_abs_delta_predicted += abs_d;
            if (abs_d > m_stats.max_abs_delta_predicted)
                m_stats.max_abs_delta_predicted = abs_d;
            if (se.entry->delta_vpn > 0) m_stats.positive_delta_predictions++;
            else                         m_stats.negative_delta_predictions++;

            // ---- Stat: per-slot ----
            if (current_depth == 0 && se.slot_idx < TPTE_MAX_SLOTS)
                m_stats.slot_prediction_count[se.slot_idx]++;
            
            // Track if this prediction came from the global slot
            if (current_depth == 0 && m_reserve_global_slot && entries.size() >= 2)
            {
                uint32_t global_slot_idx = static_cast<uint32_t>(entries.size()) - 1;
                if (se.slot_idx == global_slot_idx)
                    m_stats.global_slot_predictions++;
            }
            
            // ---- Chained prefetching: enqueue next depth ----
            // Pass the child's path score so the next level can compose further.
            if (chained_payload != 0 && m_max_prefetch_depth > 0)
            {
                m_stats.chained_prefetch_opportunities++;
                if (current_depth < m_max_prefetch_depth)
                {
                    // Chain can only start after the current region's PTW completes
                    work_queue.push_back({target_region_u64, chained_payload, current_depth + 1, region_completion, child_score});
                    if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG))
                    {
                        std::ostringstream oss;
                        oss << "  [D" << current_depth << "] CHAIN_ENQUEUE region=0x" << std::hex << target_region_u64
                            << std::dec << " next_depth=" << (current_depth + 1)
                            << " child_score=" << child_score
                            << " payload=" << m_codec.dumpPayload(chained_payload);
                        m_sim_log.debug(oss.str());
                    }
                }
                else
                {
                    m_stats.chained_prefetch_skipped_max_depth++;
                }
            }

            if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG))
            {
                std::ostringstream oss;
                oss << "  [D" << current_depth << "] ISSUED region=0x" << std::hex << predicted_region
                    << " base_vpn=0x" << base_vpn_of_region << std::dec
                    << " delta=" << se.entry->delta_vpn
                    << " conf=" << se.entry->conf
                    << " slot=" << se.slot_idx;
                m_sim_log.debug(oss.str());
            }
        }
        else
        {
            if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG))
            {
                std::ostringstream oss;
                oss << "  [D" << current_depth << "] ALL_PAGES_FAILED region=0x" << std::hex << predicted_region
                    << std::dec
                    << " delta=" << se.entry->delta_vpn
                    << " conf=" << se.entry->conf
                    << " slot=" << se.slot_idx;
                m_sim_log.debug(oss.str());
            }
        }
        } // end for (sorted slots)

    } // end while (work_queue)

    // Restore shmem time to pre-prefetch state (prefetch walks are "transparent")
    shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, original_shmem_time);

    // Summary stats (use local counters from this call, not cumulative stats)
    if (regions_issued_this_call > 0)
    {
        m_stats.queries_with_at_least_one_prefetch++;
#if ENABLE_TPTE_UNIQUE_VPN_TRACKING
        m_unique_vpns_prefetched_for.insert(vpn);
#endif

        if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG))
        {
            m_sim_log.debug("  SUMMARY regions_this_call=", regions_issued_this_call,
                            " chained_this_call=", chained_issued_this_call,
                            " total_pages=", result.size());
        }
    }
    else
    {
        if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG) && pte_payload != 0)
            m_sim_log.debug("  SUMMARY no_regions_issued (payload was non-zero)");
    }

    } // end if (!tlb_hit || (pq_hit && m_prefetch_on_hit))

    // ----------------------------------------------------------------
    // PHASE 2: Learning / update path (region-level)
    //
    // Gating policy (mirrors prefetch gating):
    //   - TLB miss        → always learn
    //   - Regular TLB hit → never learn (we don't see the transition)
    //   - PQ hit          → learn only if m_learn_on_hit
    //
    // m_last_region_id is always updated (bottom of this function) so
    // the next access sees the correct previous region even if learning
    // was gated off this time.
    //
    // PC table resolution ordering is critical:
    //   We resolve the PC entry ONCE here, BEFORE the learning gate,
    //   to capture the stride prev_vpn before learning overwrites
    //   pc_entry.last_vpn.  This ensures:
    //     (a) stride prev_vpn is the value from the PREVIOUS access
    //     (b) stride state survives PC table evictions (via backing store)
    //     (c) the entry is in the SRAM cache for Phase 3's stride pointers
    //
    // NOTE: pc_entry.last_vpn stores the raw VPN (4KB granularity) so
    //       that stride detection captures fine-grained page-level
    //       patterns.  Learning derives region_id via >> region_shift.
    uint64_t stride_pc_prev_vpn = 0;
    bool     stride_pc_prev_valid  = false;

    if (m_mode == TemporalPTEMode::PC_COND_LEARN && !m_pc_table.empty())
    {
        // Set transient context for backing store cache access modeling
        m_backing_lock    = lock;
        m_backing_modeled = modeled;
        m_backing_count   = count;

        auto& pc_entry = resolvePCEntry(eip);
        // resolvePCEntry guarantees pc_entry.valid && pc_entry.pc_tag matches
        stride_pc_prev_vpn = pc_entry.last_vpn;       // raw vpn (4KB)
        stride_pc_prev_valid  = (pc_entry.last_vpn != 0);
        // Do NOT update last_vpn here — it must only advance on misses
        // (inside the learning gate below) to stay in sync with PTE
        // payload learning.
    }

    if ((!tlb_hit || (pq_hit && m_learn_on_hit)) && m_mode != TemporalPTEMode::PTE_ONLY)
    {
        uint64_t src_region = 0;
        int src_page_size = 12;
        m_current_is_pc_miss = false;  // Reset transient flag

        if (m_mode == TemporalPTEMode::PC_COND_LEARN && !m_pc_table.empty())
        {
            // Entry already resolved above.
            // Derive region from the saved prev VPN for learning.
            uint32_t idx = pcHash(eip) & m_pc_table_mask;
            src_page_size = m_pc_table[idx].last_page_size;
            if (src_page_size == 0) src_page_size = 12;  // default for uninitialized

            if (src_page_size == 21)
                src_region = ((stride_pc_prev_vpn >> 9) >> m_region_shift) | REGION_2MB_TAG;
            else
                src_region = stride_pc_prev_vpn >> m_region_shift;

            m_current_is_pc_miss = !stride_pc_prev_valid;
            // Update last_vpn (raw 4KB vpn) and page size only on misses
            m_pc_table[idx].last_vpn = vpn;  // store raw vpn (4KB)
            m_pc_table[idx].last_page_size = page_size;
        }
        else
        {
            // Simple mode: use the global last_vpn (which now tracks region_id)
            src_region = m_last_region_id;
        }

        if (src_region == region_id && src_region != 0)
        {
            m_stats.learning_same_page++;
            if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
                m_sim_log.trace("  LEARN: skip same_region=0x", SimLog::hex(region_id));
        }
        else if (src_region != 0 && src_region != region_id)
        {
            // Check page size compatibility for learning
            bool both_2mb = is_2mb && (src_page_size == 21);
            bool both_4kb = !is_2mb && (src_page_size != 21);

            if (both_2mb || both_4kb)
            {
                m_stats.learning_opportunities++;
                if (both_2mb)
                    m_stats.learning_transitions_2mb++;
                if (instruction)
                    m_stats.learning_opportunities_instruction++;
                else
                    m_stats.learning_opportunities_data++;
                if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG))
                {
                    std::ostringstream oss;
                    oss << "  LEARN: transition src_region=0x" << std::hex << (src_region & ~REGION_2MB_TAG)
                        << " -> curr_region=0x" << (region_id & ~REGION_2MB_TAG) << std::dec
                        << " delta=" << (static_cast<int64_t>(region_id & ~REGION_2MB_TAG) - static_cast<int64_t>(src_region & ~REGION_2MB_TAG))
                        << (both_2mb ? " [2MB]" : " [4KB]");
                    m_sim_log.debug(oss.str());
                }
                learnTransition(src_region, region_id, eip, pt);
            }
            else
            {
                // Mixed page sizes (4KB→2MB or 2MB→4KB): skip learning
                m_stats.learning_skipped_mixed_pagesize++;
            }
        }
    }

    // ----------------------------------------------------------------
    // PHASE 3: Stride detection + direct prefetch
    //
    // Complements temporal delta prediction with a simple stride detector.
    // Temporal deltas capture irregular successor patterns (A→B, A→C, ...),
    // while stride captures regular sequential patterns (A, A+s, A+2s, ...).
    //
    // Stride state:
    //   last_delta  — previous page-to-page delta
    //   confidence  — how many consecutive times the same delta repeated
    //   value       — the confirmed stride (= last_delta when conf >= threshold)
    //
    // Per-PC tracking (PC_COND_LEARN):
    //   Stride state lives inside the PCTableEntry (which survives evictions
    //   via the backing store).  This prevents interleaved streams from
    //   different PCs from polluting each other's stride patterns.
    //   Deltas are computed at 4KB page granularity for fine-grained capture.
    //
    // Global fallback (non-PC modes):
    //   A single global stride detector.  Deltas are region-level (not page-
    //   level) since there's no PC isolation.
    //
    // When confidence reaches stride_conf_threshold, strideDirectPrefetch()
    // issues stride_direct_degree lookahead walks along the detected stride.
    // ----------------------------------------------------------------
    
    if (m_stride_direct_prefetch && m_mode != TemporalPTEMode::PTE_ONLY)
    {
        // Resolve per-PC or global stride state pointers
        int64_t  *p_last_delta;
        uint32_t *p_confidence;
        int64_t  *p_value;
        uint64_t  prev_vpn_for_stride;
        bool      has_prev;

        if (m_mode == TemporalPTEMode::PC_COND_LEARN && !m_pc_table.empty())
        {
            uint32_t idx = pcHash(eip) & m_pc_table_mask;
            auto& pc_entry = m_pc_table[idx];
            p_last_delta = &pc_entry.stride_last_delta;
            p_confidence = &pc_entry.stride_confidence;
            p_value      = &pc_entry.stride_value;
            // Use the per-PC prev VPN (4KB) saved before section 3
            prev_vpn_for_stride = stride_pc_prev_vpn;
            has_prev            = stride_pc_prev_valid;
        }
        else
        {
            // Global fallback (still region-level for non-PC modes)
            p_last_delta = &m_stride_last_delta;
            p_confidence = &m_stride_confidence;
            p_value      = &m_stride_value;
            prev_vpn_for_stride = m_last_region_id;
            has_prev            = m_stride_has_prev;
        }

        if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
        {
            std::ostringstream oss;
            oss << "  STRIDE: eip=0x" << std::hex << eip
                << " vpn=0x" << vpn
                << " prev_vpn=0x" << prev_vpn_for_stride << std::dec
                << " has_prev=" << has_prev
                << " conf=" << *p_confidence
                << " last_delta=" << *p_last_delta
                << " value=" << *p_value;
            m_sim_log.trace(oss.str());
        }

        if (prev_vpn_for_stride != 0 && vpn != prev_vpn_for_stride)
        {
            // Page-level delta (4KB granularity) for per-PC,
            // region-level for global fallback
            int64_t curr_delta = static_cast<int64_t>(vpn) - static_cast<int64_t>(prev_vpn_for_stride);

            // For global fallback (which still stores region_id), compute
            // region-level delta instead
            if (m_mode != TemporalPTEMode::PC_COND_LEARN || m_pc_table.empty())
                curr_delta = static_cast<int64_t>(region_id) - static_cast<int64_t>(prev_vpn_for_stride);

            if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
                m_sim_log.trace("  STRIDE: curr_delta=", curr_delta, "last_delta=", *p_last_delta,
                                "conf=", *p_confidence, "value=", *p_value);

            if (has_prev || *p_confidence > 0)
            {
                if (curr_delta == *p_last_delta)
                {
                    // Same delta repeated — increase confidence
                    if (*p_confidence < m_stride_conf_threshold)
                        (*p_confidence)++;
                    *p_value = curr_delta;

                    if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG))
                        m_sim_log.debug("  STRIDE_MATCH: delta=", curr_delta,
                                        "new_conf=", *p_confidence, "thresh=", m_stride_conf_threshold);

                    if (*p_confidence >= m_stride_conf_threshold)
                    {
                        m_stats.stride_detected++;
                        if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG))
                            m_sim_log.debug("  STRIDE_DETECT: stride=", *p_value, "pages, issuing prefetch");

                        // --- Direct stride prefetch (page-granularity, no alignment needed) ---
                        // In global fallback mode, *p_value is a region-level delta;
                        // convert to page-level for strideDirectPrefetch which
                        // always operates in page units.
                        int64_t stride_in_pages = *p_value;
                        if (m_mode != TemporalPTEMode::PC_COND_LEARN || m_pc_table.empty())
                            stride_in_pages *= static_cast<int64_t>(m_region_pages);
                        auto stride_pfs = strideDirectPrefetch(vpn, stride_in_pages,
                                                                *p_confidence,
                                                                eip, lock, modeled, count, pt);
                        for (auto& sq : stride_pfs)
                            result.push_back(sq);
                    }
                }
                else
                {
                    // Delta changed — reset or track the new one
                    if (*p_value != 0 && curr_delta != *p_value)
                        m_stats.stride_changed++;
                    m_stats.stride_reset++;
                    if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
                        m_sim_log.trace("  STRIDE_RESET: old_value=", *p_value,
                                        "new_delta=", curr_delta);
                    *p_confidence = 1;
                    *p_value = curr_delta;
                }
            }
            else
            {
                // First transition for this PC / global
                if (m_mode != TemporalPTEMode::PC_COND_LEARN || m_pc_table.empty())
                    m_stride_has_prev = true;
                *p_value = curr_delta;
                *p_confidence = 1;
                if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
                    m_sim_log.trace("  STRIDE_FIRST: delta=", curr_delta);
            }

            *p_last_delta = curr_delta;
        }
    }

    // Update global last trackers (store region_id, not raw vpn)
    m_last_region_id = region_id;
    m_last_pc = static_cast<uint64_t>(eip);

    // Periodically flush unique-page counts (every 10000 queries)
#if ENABLE_TPTE_PERIODIC_FINALIZE
    if ((m_stats.queries % 10000) == 0)
        finalizeUniquePageStats();
#endif

    // --- OS-managed side-table variant ---
    // The temporal payload lives in a separate OS-managed table in memory,
    // fetched IN PARALLEL with the page-table walk rather than riding in spare
    // PTE bits.  The walk (and thus translation latency) is unaffected; the
    // prefetch may only be *issued* once the parallel table access completes.
    // Model that single access here and push every generated prefetch's
    // materialization timestamp out by the access latency.
    if (m_payload_in_side_table && !result.empty())
    {
        const SubsecondTime side_lat =
            modelSideTableAccess(region_id, eip, lock, modeled, count);
        if (side_lat > SubsecondTime::Zero())
            for (auto& q : result)
                q.timestamp += side_lat;
    }

    // Cache-only ablation: the prefetch PTWs issued above have already warmed
    // the predicted PTE lines in the data cache; suppress PQ/TLB installation
    // so a later demand access still walks but hits the warmed lines (cheaper
    // walk rather than an eliminated one).  Isolates the cache-warming benefit.
    if (!m_prefetch_install_pq)
        result.clear();

    return result;
}

// ============================================================================
// Stride direct prefetch
// ============================================================================
//
// When the stride detector confirms a stable page-level stride (same delta
// repeated stride_conf_threshold times), this function issues PTWTransparent
// walks for the next stride_direct_degree pages along the stride.
//
// Unlike temporal delta predictions (which operate at region granularity),
// stride prefetches are page-granular and do not fan out — each prefetched
// page gets exactly one modeled PTW walk.
//
// The stride is in page units (4KB).  For the global fallback (non-PC mode),
// the caller converts from region-level stride to page-level before calling.
//
// Each successful stride prefetch is tracked in the inflight map with
// PrefetchSource::STRIDE so timeliness stats can distinguish stride
// predictions from temporal delta predictions.
// ============================================================================

std::vector<query_entry> TemporalPTEPrefetcher::strideDirectPrefetch(
        uint64_t vpn, int64_t stride_pages,
        uint32_t stride_confidence,
        IntPtr eip, Core::lock_signal_t lock,
        bool modeled, bool count, PageTable *pt)
{
    std::vector<query_entry> pf_result;
    if (stride_pages == 0 || m_stride_direct_degree == 0)
        return pf_result;

    if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG))
    {
        std::ostringstream oss;
        oss << "  STRIDE_PREFETCH: vpn=0x" << std::hex << vpn << std::dec
            << " stride=" << stride_pages << " pages"
            << " conf=" << stride_confidence
            << " degree=" << m_stride_direct_degree
            << " eip=0x" << std::hex << eip;
        m_sim_log.debug(oss.str());
    }

    SubsecondTime t_before = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

    for (uint32_t i = 1; i <= m_stride_direct_degree; i++)
    {
        int64_t future_vpn = static_cast<int64_t>(vpn) + static_cast<int64_t>(i) * stride_pages;
        if (future_vpn <= 0)
            continue;

        IntPtr future_addr = static_cast<IntPtr>(static_cast<uint64_t>(future_vpn)) << m_page_shift;
        query_entry q = PTWTransparent(future_addr, eip, lock, modeled, count, pt);
        m_stats.stride_direct_issued++;

        if (q.ppn != 0)
        {
            pf_result.push_back(q);
            m_stats.stride_direct_successful++;

            // ---- Inflight tracking (region granularity) ----
            uint64_t target_vpn = static_cast<uint64_t>(future_vpn);
            uint64_t target_region = target_vpn >> m_region_shift;
            insertInflight(target_region, t_before.getNS(), q.timestamp.getNS(),
                           PrefetchSource::STRIDE, stride_confidence, /*depth=*/0);

            // ---- Walk latency tracking ----
            SubsecondTime walk_latency_ss = (q.timestamp > t_before)
                                            ? (q.timestamp - t_before)
                                            : SubsecondTime::Zero();
            UInt64 walk_fs = walk_latency_ss.getFS();
            m_stats.total_prefetch_walk_latency_fs += walk_fs;
            if (walk_fs > 0 && walk_fs < m_stats.min_prefetch_walk_latency_fs)
                m_stats.min_prefetch_walk_latency_fs = walk_fs;
            if (walk_fs > m_stats.max_prefetch_walk_latency_fs)
                m_stats.max_prefetch_walk_latency_fs = walk_fs;

#if ENABLE_TPTE_UNIQUE_VPN_TRACKING
            m_unique_predicted_vpns.insert(target_vpn);
#endif
        }
        else
        {
            m_stats.stride_direct_failed++;
        }
    }
    return pf_result;
}

// ============================================================================
// Learning: update the source region's PTE payload with the observed delta
// ============================================================================
//
// Called when a region-to-region transition is observed (src → curr).
// The goal is to encode the delta (curr - src) into the source region's
// PTE payload so that future accesses to src can predict curr.
//
// Algorithm:
//   1. Compute signed delta = curr_region - src_region
//   2. Check that delta fits in the codec's offset_bits (two's complement)
//   3. Read the source region's current payload and decode all slots
//   4. Determine which slots to use:
//      - With reserve_global_slot: PC hits use slots 0..N-2, PC misses
//        use only slot N-1 (the "global" slot for non-PC-specific patterns)
//      - Without reserve_global_slot: all slots are available
//   5. Search for existing slot with matching delta → bump confidence
//   6. If not found → evict a victim slot (lowest confidence / random)
//      and insert the new delta with conf_init
//   7. Apply confidence policy (competitive decay of other slots, etc.)
//   8. Encode all slots back and write the payload to the page table
//   9. Optional periodic decay: every decay_period updates, halve all
//      confidences in the source page's payload
//
// The payload is anchored at the first page of the source region
// (src_region << region_shift), so all pages in a region share the
// same prediction metadata.
//
// NOTE: src_region and curr_region are region IDs (vpn >> region_shift).
// ============================================================================

void TemporalPTEPrefetcher::learnTransition(uint64_t src_region, uint64_t curr_region,
                                             IntPtr eip, PageTable *pt)
{
    // Detect 2MB plane from the REGION_2MB_TAG bit
    bool learn_2mb = (src_region & REGION_2MB_TAG) != 0;
    uint64_t src_raw  = src_region  & ~REGION_2MB_TAG;
    uint64_t curr_raw = curr_region & ~REGION_2MB_TAG;

    int64_t delta_signed = static_cast<int64_t>(curr_raw) - static_cast<int64_t>(src_raw);

    // Check that delta fits in offset_bits (two's complement range)
    const uint32_t ob = m_codec.config().offset_bits;
    int64_t max_delta = (1LL << (ob - 1)) - 1;
    int64_t min_delta = -(1LL << (ob - 1));
    if (delta_signed > max_delta || delta_signed < min_delta)
    {
        m_stats.learning_delta_out_of_range++;
        if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG))
        {
            std::ostringstream oss;
            oss << "  LEARN: delta_out_of_range delta=" << delta_signed
                << " range=[" << min_delta << "," << max_delta << "]"
                << (learn_2mb ? " [2MB]" : " [4KB]");
            m_sim_log.debug(oss.str());
        }
        return;  // Delta out of range — cannot represent (inter-region jump)
    }

    int32_t delta = static_cast<int32_t>(delta_signed);

    // The payload is stored per-VPN; use the first page of the source region.
    // For 2MB plane: use PMD payload keyed by 2MB VPN.
    // For 4KB plane: use PTE payload keyed by 4KB VPN.
    uint64_t src_vpn = src_raw << m_region_shift;

    PayloadWord old_payload = learn_2mb
        ? pt->readPMDPayloadBits(src_vpn)
        : pt->readPayloadBits(src_vpn);
    auto entries = m_codec.decode(old_payload);

    // Determine slot range based on reserve_global_slot and PC miss status
    // If reserve_global_slot is enabled:
    //   - PC hit:  use slots 0 to N-2 (PC-conditioned slots)
    //   - PC miss: use only slot N-1 (global slot)
    // Otherwise: use all slots normally
    uint32_t slot_start = 0;
    uint32_t slot_end = static_cast<uint32_t>(entries.size());
    bool use_global_slot_only = false;

    if (m_reserve_global_slot && entries.size() >= 2)
    {
        uint32_t global_slot_idx = static_cast<uint32_t>(entries.size()) - 1;
        
        if (m_current_is_pc_miss)
        {
            // PC miss: only use the global slot (last slot)
            slot_start = global_slot_idx;
            slot_end = global_slot_idx + 1;
            use_global_slot_only = true;
            
            if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
                m_sim_log.trace("    GLOBAL_SLOT: PC miss, using slot=", global_slot_idx);
        }
        else
        {
            // PC hit: use all slots except the last one (reserve for global)
            slot_end = global_slot_idx;
            
            if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
                m_sim_log.trace("    PC_SLOTS: using slots 0-", global_slot_idx - 1);
        }
    }

    // Check if this delta already exists in the allowed slot range
    bool found = false;
    uint32_t found_slot = slot_start;
    for (uint32_t i = slot_start; i < slot_end; i++)
    {
        if (entries[i].delta_vpn == delta)
        {
            found = true;
            found_slot = i;
            break;
        }
    }

    // Delegate bump + decay to the pluggable confidence policy.
    // The policy mutates entry confidences and reports what happened.
    uint32_t max_c = m_codec.config().maxConf();
    ConfPolicyResult conf_result = m_confidence_policy->apply(
        entries, slot_start, slot_end, found, found_slot, max_c);

    // Update stats based on policy result
    if (found)
    {
        m_stats.learning_bumps++;
        if (found_slot < TPTE_MAX_SLOTS)
            m_stats.slot_bump_count[found_slot]++;
        if (use_global_slot_only)
            m_stats.global_slot_bumps++;

        if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
            m_sim_log.trace("    BUMP slot=", found_slot, "delta=", delta,
                            "new_conf=", entries[found_slot].conf);
    }
    if (conf_result.any_decayed)
    {
        if (found)
            m_stats.competitive_decay_on_bump++;
        else
            m_stats.competitive_decay_on_miss++;
    }

    if (!found)
    {
        // Insert into a victim slot within the allowed range
        // (victim selection benefits from the decay above — stale slots
        //  now have lower confidence and are more likely to be evicted)
        uint32_t victim = slot_start;

        if (use_global_slot_only)
        {
            // For global slot, always use the reserved slot
            victim = slot_start;  // which is the global slot index
            m_stats.global_slot_updates++;
        }
        else
        {
            // Normal replacement within allowed range
            switch (m_replacement)
            {
            case TemporalPTEReplacement::LOWEST_CONF:
            {
                uint32_t min_c = entries[slot_start].conf;
                victim = slot_start;
                for (uint32_t i = slot_start + 1; i < slot_end; i++)
                {
                    if (entries[i].conf < min_c)
                    {
                        min_c = entries[i].conf;
                        victim = i;
                    }
                }
                break;
            }
            case TemporalPTEReplacement::RANDOM:
            {
                uint32_t range = slot_end - slot_start;
                victim = slot_start + (m_rng() % range);
                break;
            }
            case TemporalPTEReplacement::LRU:
                // Fallback to lowest-conf (LRU needs age bits not yet implemented)
            {
                uint32_t min_c = entries[slot_start].conf;
                victim = slot_start;
                for (uint32_t i = slot_start + 1; i < slot_end; i++)
                {
                    if (entries[i].conf < min_c)
                    {
                        min_c = entries[i].conf;
                        victim = i;
                    }
                }
                break;
            }
            }
        }

        entries[victim].delta_vpn = delta;
        entries[victim].conf = m_conf_init;
        m_stats.learning_inserts++;
        if (victim < TPTE_MAX_SLOTS)
            m_stats.slot_replace_count[victim]++;

        if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
            m_sim_log.trace("    INSERT victim_slot=", victim, "delta=", delta, "conf_init=", m_conf_init);
    }

    // Write back to the first page of the source region
    PayloadWord new_payload = m_codec.encodeAll(old_payload, entries);
    if (learn_2mb)
        pt->writePMDPayloadBits(src_vpn, new_payload);
    else
        pt->writePayloadBits(src_vpn, new_payload);
    m_stats.learning_updates++;

    // Record this in-PTE payload update so the MMU can dirty the source PTE's
    // cacheline after the demand walk completes (-> models writeback traffic).
    // Only for the PTE-embedded variant; the side-table variant stores payloads
    // out of line (handled separately).
    if (m_model_payload_writeback && !m_payload_in_side_table)
        m_pending_payload_wb.emplace_back(src_vpn, learn_2mb);
    if (m_current_is_instruction)
        m_stats.learning_updates_instruction++;
    else
        m_stats.learning_updates_data++;

    // --- Debug: log full learn result ---
    if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG))
    {
        std::ostringstream oss;
        oss << "  LEARN: " << (found ? "BUMP" : "INSERT")
            << " src_region=0x" << std::hex << src_region
            << " -> curr_region=0x" << curr_region << std::dec
            << " delta=" << delta
            << " new_payload=" << m_codec.dumpPayload(new_payload);
        m_sim_log.debug(oss.str());
    }

    // ---- Stat: track unique regions whose payload was learned ----
#if ENABLE_TPTE_UNIQUE_VPN_TRACKING
    m_unique_vpns_learned.insert(src_region);
#endif

    // Confidence decay
    if (m_enable_decay && m_decay_period > 0)
    {
        m_update_counter++;
        if (m_update_counter >= m_decay_period)
        {
            m_update_counter = 0;
            // Decay the source page payload we just wrote
            PayloadWord decayed = m_codec.decayAllConf(new_payload);
            if (learn_2mb)
                pt->writePMDPayloadBits(src_vpn, decayed);
            else
                pt->writePayloadBits(src_vpn, decayed);
            m_stats.decay_events++;

            if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG))
                m_sim_log.debug("  DECAY: src_vpn=", SimLog::hex(src_vpn),
                                "decayed_payload=", m_codec.dumpPayload(decayed));
        }
    }
}

// ============================================================================
// resolvePCEntry — Unified PC table lookup with write-back backing store
// ============================================================================
//
// The PC table is a direct-mapped cache indexed by pcHash(eip).  Each entry
// stores the last VPN accessed by that PC (for learning) and per-PC stride
// detector state (last_delta, confidence, stride_value).
//
// Without virtualization (virtualize_pc_table=false):
//   Simple direct-mapped table.  On tag mismatch, the old entry is silently
//   evicted — its stride state and last_vpn are lost.
//
// With virtualization (virtualize_pc_table=true):
//   The SRAM cache (m_pc_table) acts as a write-back cache in front of a
//   5-level radix-tree backing store in emulated physical memory.
//
//   On cache miss:
//     1. Write-back: evicted entry → pcRadixInsert (radix tree)
//     2. Probe: pcRadixFind for the requested PC
//        - Hit: promote into SRAM cache (restore all state)
//        - Miss: cold start (fresh entry)
//
//   Each radix node is a 512-entry frame occupying one 4KB physical page.
//   Traversal emits cache accesses (emitBackingStoreAccess) with PWC
//   lookups at each level, modeling the real hardware cost of a
//   page-table-like walk through the backing store.
//
//   Leaf entries are packed into 64 bits (packPCData/unpackPCData):
//     [35:0] last_vpn, [36] valid, [46:37] stride_last_delta (10-bit signed),
//     [50:47] stride_confidence (4-bit), [60:51] stride_value (10-bit signed)
//
// Guarantees: on return, m_pc_table[idx] is valid and matches pcTag(eip).
// ============================================================================

TemporalPTEPrefetcher::PCTableEntry&
TemporalPTEPrefetcher::resolvePCEntry(IntPtr eip)
{
    uint32_t idx = pcHash(eip) & m_pc_table_mask;
    auto& cached = m_pc_table[idx];
    uint64_t tag = pcTag(eip);

    // Fast path: cache hit
    if (cached.valid && cached.pc_tag == tag)
    {
        m_stats.pc_table_hits++;
        if (m_sim_log.isEnabled(SimLog::LEVEL_TRACE))
        {
            std::ostringstream oss;
            oss << "  PC_CACHE_HIT: eip=0x" << std::hex << eip
                << " idx=" << std::dec << idx
                << " last_vpn=0x" << std::hex << cached.last_vpn << std::dec
                << " stride_conf=" << cached.stride_confidence
                << " stride_val=" << cached.stride_value;
            m_sim_log.trace(oss.str());
        }
        return cached;
    }

    // Cache miss
    if (m_virtualize_pc_table)
    {
        // Write-back the evicted entry to the radix-tree backing store
        if (cached.valid)
        {
            uint64_t evict_key = cached.pc_tag;
            if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG))
            {
                std::ostringstream oss;
                oss << "  PC_BACKING_WB: evict tag=0x" << std::hex << cached.pc_tag
                    << " idx=" << std::dec << idx
                    << " last_vpn=0x" << std::hex << cached.last_vpn << std::dec
                    << " stride_conf=" << cached.stride_confidence
                    << " stride_val=" << cached.stride_value;
                m_sim_log.debug(oss.str());
            }
            pcRadixInsert(evict_key, cached);
            m_stats.pc_backing_writebacks++;
        }

        // Probe radix-tree backing store for the requested PC
        uint64_t lookup_key = tag;
        PCTableEntry found_entry;
        if (pcRadixFind(lookup_key, found_entry))
        {
            // Backing hit — promote into SRAM cache.  Restore pc_tag
            // (not stored in the packed 64-bit leaf).
            cached = found_entry;
            cached.pc_tag = tag;
            m_stats.pc_backing_hits++;
            if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG))
            {
                std::ostringstream oss;
                oss << "  PC_BACKING_HIT: eip=0x" << std::hex << eip
                    << " idx=" << std::dec << idx
                    << " restored last_vpn=0x" << std::hex << cached.last_vpn << std::dec
                    << " stride_conf=" << cached.stride_confidence
                    << " stride_val=" << cached.stride_value;
                m_sim_log.debug(oss.str());
            }
        }
        else
        {
            // Cold miss — install fresh entry
            cached.valid = true;
            cached.pc_tag = tag;
            cached.last_vpn = 0;
            cached.stride_last_delta = 0;
            cached.stride_confidence = 0;
            cached.stride_value = 0;
            m_stats.pc_backing_misses++;
            if (m_sim_log.isEnabled(SimLog::LEVEL_DEBUG))
            {
                std::ostringstream oss;
                oss << "  PC_BACKING_MISS: eip=0x" << std::hex << eip
                    << " idx=" << std::dec << idx << " (cold)";
                m_sim_log.debug(oss.str());
            }
        }
    }
    else
    {
        // No virtualization — simple overwrite (stride state lost on eviction)
        if (cached.valid && cached.pc_tag != tag)
            m_stats.pc_table_evictions++;
        m_stats.pc_table_misses++;
        cached.valid = true;
        cached.pc_tag = tag;
        cached.last_vpn = 0;
        cached.stride_last_delta = 0;
        cached.stride_confidence = 0;
        cached.stride_value = 0;
    }

    return cached;
}

// ============================================================================
// Pack / unpack helpers for 64-bit radix leaf entries
// ============================================================================

uint64_t TemporalPTEPrefetcher::packPCData(const PCTableEntry& e)
{
    // If either signed stride field doesn't fit in 10 bits (range −512..+511),
    // invalidate all stride state: set confidence/delta/value to 0.
    // The entry will re-learn the stride after recovery — better than
    // silently truncating and producing a wrong stride.
    static constexpr int64_t S10_MIN = -512;
    static constexpr int64_t S10_MAX =  511;
    bool stride_fits = (e.stride_last_delta >= S10_MIN && e.stride_last_delta <= S10_MAX)
                    && (e.stride_value      >= S10_MIN && e.stride_value      <= S10_MAX);

    uint64_t w = 0;
    w |= (static_cast<uint64_t>(e.last_vpn) & 0xFFFFFFFFFULL);         // bits [35:0]
    w |= (static_cast<uint64_t>(e.valid ? 1 : 0) << 36);               // bit  [36]
    if (stride_fits)
    {
        w |= (static_cast<uint64_t>(e.stride_last_delta) & 0x3FF) << 37;   // bits [46:37]
        w |= (static_cast<uint64_t>(e.stride_confidence) & 0xF)   << 47;   // bits [50:47]
        w |= (static_cast<uint64_t>(e.stride_value)      & 0x3FF) << 51;   // bits [60:51]
    }
    // else: stride fields are all 0 → confidence=0, will re-learn after recovery
    return w;
}

void TemporalPTEPrefetcher::unpackPCData(uint64_t w, PCTableEntry& e)
{
    e.last_vpn          = w & 0xFFFFFFFFFULL;                           // bits [35:0]
    e.valid             = ((w >> 36) & 1) != 0;                         // bit  [36]
    // Sign-extend 10-bit two's complement fields
    uint64_t raw_delta  = (w >> 37) & 0x3FF;
    e.stride_last_delta = static_cast<int64_t>(raw_delta | ((raw_delta & 0x200) ? ~0x3FFULL : 0));
    e.stride_confidence = static_cast<uint32_t>((w >> 47) & 0xF);      // bits [50:47]
    uint64_t raw_value  = (w >> 51) & 0x3FF;
    e.stride_value      = static_cast<int64_t>(raw_value | ((raw_value & 0x200) ? ~0x3FFULL : 0));
    e.pc_tag            = 0;  // Not stored in backing store
}

// ============================================================================
// Radix-tree backing store (emulates physical-memory-resident structure)
// ============================================================================
//
// The radix tree provides an "unlimited" backing store for the PC table,
// modeling a software structure resident in physical memory.
//
// Structure: 5-level radix tree, 9 bits per level (512-way fanout)
//   → 45-bit key space (covers the lower 45 bits of the PC)
//   → each frame = 512 entries × 8 bytes = 4 KB = one physical page
//   → frames are lazily allocated (memory ∝ unique PCs, not key space)
//
// Access modeling:
//   Each internal/leaf frame access calls emitBackingStoreAccess() which:
//     1. Checks the page-walk cache (PWC) — if hit, no cache access needed
//     2. On PWC miss, issues a real cache hierarchy access via
//        mmu->accessCache() with is_prefetch=false (demand path), so the
//        backing store data competes for L1D/L2/LLC space just like real
//        page table data
//
//   PWC level assignment mirrors a real page table walk:
//     root = level 4, intermediate = levels 3/2 (clamped), leaf = level 0
//
// pcRadixInsert: walk to leaf, lazily allocate frames, write packed entry
// pcRadixFind:   walk to leaf, return unpacked entry if valid
// pcRadixCleanup: recursive deallocation (called from destructor)
// ============================================================================

TemporalPTEPrefetcher::PCRadixFrame*
TemporalPTEPrefetcher::pcRadixAllocFrame(bool is_leaf_level)
{
    PCRadixFrame* f = new PCRadixFrame;
    f->entries = new PCRadixEntry[PC_RADIX_FANOUT];

    // Allocate emulated physical memory (one 4 KB page per frame)
    MimicOS* os = Sim()->getMimicOS();
    f->emulated_ppn = os->getMemoryAllocator()->handle_page_table_allocations(4096);

    for (uint32_t i = 0; i < PC_RADIX_FANOUT; i++)
    {
        f->entries[i].is_leaf = is_leaf_level;
        if (is_leaf_level)
        {
            f->entries[i].u.leaf.valid  = false;
            f->entries[i].u.leaf.packed = 0;
        }
        else
            f->entries[i].u.next_level = nullptr;
    }

    m_stats.pc_radix_frames_allocated++;
    m_stats.pc_radix_bytes = m_stats.pc_radix_frames_allocated * 4096;
    return f;
}

// Issue a cache access for reading/writing a radix frame slot in emulated physical memory.
// First checks the page-walk cache (PWC); on PWC miss falls through to accessCache (L1D).
// accessCache saves/restores shmem time so no latency is charged.
void
TemporalPTEPrefetcher::emitBackingStoreAccess(IntPtr emulated_ppn, uint32_t index, int pwc_level)
{
    // Physical address: page base + slot offset (8 bytes per entry, same as pagetable_radix)
    IntPtr phys_addr = (emulated_ppn << 12) | (static_cast<IntPtr>(index) * 8);

    // Check PWC first (like a real page table walk)
    auto *mmu = static_cast<MemoryManagementUnitBase*>(memory_manager->getMMU());
    if (mmu == nullptr)
        return;  // MMU not yet initialized — skip modeling
    BaseFilter* filter = mmu->getPTWFilter();
    if (filter != nullptr)
    {
        SubsecondTime t_now = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);
        bool pwc_hit = filter->lookupPWC(phys_addr, t_now, pwc_level, m_backing_count);
        if (pwc_hit)
        {
            m_stats.pc_backing_pwc_hits++;
            return;  // PWC absorbed this access
        }
    }

    // PWC miss — go to cache hierarchy
    MemoryManagementUnitBase::translationPacket pkt(
        phys_addr,
        0,                                          // eip (not meaningful for backing store access)
        false,                                      // instruction
        m_backing_lock,
        m_backing_modeled,
        m_backing_count,
        CacheBlockInfo::block_type_t::PAGE_TABLE_DATA
    );

    SubsecondTime t_now = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);
    HitWhere::where_t hit_where;
    SubsecondTime latency = mmu->accessCache(pkt, t_now, false, hit_where);

    m_stats.pc_backing_cache_accesses++;
    m_stats.pc_backing_total_latency_fs += latency.getFS();

    switch (hit_where) {
        case HitWhere::where_t::L1_OWN:
            m_stats.pc_backing_cache_l1d++;
            break;
        case HitWhere::where_t::L2_OWN:
            m_stats.pc_backing_cache_l2++;
            break;
        case HitWhere::where_t::NUCA_CACHE:
            m_stats.pc_backing_cache_nuca++;
            break;
        case HitWhere::where_t::DRAM_LOCAL:
        case HitWhere::where_t::DRAM_REMOTE:
        case HitWhere::where_t::DRAM:
            m_stats.pc_backing_cache_dram++;
            break;
        default:
            break;
    }
}

void
TemporalPTEPrefetcher::pcRadixInsert(uint64_t key, const PCTableEntry& entry)
{
    // Walk internal levels (0 .. PC_RADIX_LEVELS-2), extracting 8 bits per
    // level from the key (most-significant first).  Allocate child frames
    // lazily on the way down.
    PCRadixFrame* frame = m_pc_radix_root;
    for (uint32_t lvl = 0; lvl < PC_RADIX_LEVELS - 1; lvl++)
    {
        uint32_t shift = (PC_RADIX_LEVELS - 1 - lvl) * PC_RADIX_BITS;
        uint32_t idx   = (key >> shift) & PC_RADIX_MASK;
        // PWC level: deeper internal nodes get lower PWC levels;
        // root = PC_RADIX_LEVELS-1, clamped to min 2 so levels 2 and 3
        // share the same PWC set (avoids dedicating a PWC level to the
        // penultimate radix level which has the same access pattern).
        int pwc_level = std::max(2, static_cast<int>(PC_RADIX_LEVELS - 1 - lvl));
        emitBackingStoreAccess(frame->emulated_ppn, idx, pwc_level);

        PCRadixFrame* child = frame->entries[idx].u.next_level;
        if (child == nullptr)
        {
            bool is_leaf = (lvl == PC_RADIX_LEVELS - 2);
            child = pcRadixAllocFrame(is_leaf);
            frame->entries[idx].u.next_level = child;
        }
        frame = child;
    }

    // Leaf level: lowest 8 bits — PWC level 0
    uint32_t leaf_idx = key & PC_RADIX_MASK;
    emitBackingStoreAccess(frame->emulated_ppn, leaf_idx, /*pwc_level=*/0);

    if (!frame->entries[leaf_idx].u.leaf.valid)
    {
        m_pc_radix_entries_stored++;
        m_stats.pc_radix_entries_stored = m_pc_radix_entries_stored;
        if (m_pc_radix_entries_stored > m_stats.pc_radix_entries_peak)
            m_stats.pc_radix_entries_peak = m_pc_radix_entries_stored;
    }

    frame->entries[leaf_idx].u.leaf.valid  = true;
    frame->entries[leaf_idx].u.leaf.packed = packPCData(entry);
}

bool
TemporalPTEPrefetcher::pcRadixFind(uint64_t key, PCTableEntry& out)
{
    // Walk internal levels (0 .. PC_RADIX_LEVELS-2)
    PCRadixFrame* frame = m_pc_radix_root;
    for (uint32_t lvl = 0; lvl < PC_RADIX_LEVELS - 1; lvl++)
    {
        uint32_t shift = (PC_RADIX_LEVELS - 1 - lvl) * PC_RADIX_BITS;
        uint32_t idx   = (key >> shift) & PC_RADIX_MASK;
        int pwc_level = std::max(2, static_cast<int>(PC_RADIX_LEVELS - 1 - lvl));
        emitBackingStoreAccess(frame->emulated_ppn, idx, pwc_level);

        PCRadixFrame* child = frame->entries[idx].u.next_level;
        if (child == nullptr)
            return false;
        frame = child;
    }

    // Leaf level: lowest PC_RADIX_BITS — PWC level 0
    // With 5 levels × 9 bits = 45-bit key, every PC whose lower 45 bits
    // are unique gets its own leaf slot — no tag validation needed.
    uint32_t leaf_idx = key & PC_RADIX_MASK;
    emitBackingStoreAccess(frame->emulated_ppn, leaf_idx, /*pwc_level=*/0);
    if (frame->entries[leaf_idx].u.leaf.valid)
    {
        unpackPCData(frame->entries[leaf_idx].u.leaf.packed, out);
        return true;
    }

    return false;
}

void
TemporalPTEPrefetcher::pcRadixCleanup(PCRadixFrame* frame, uint32_t level)
{
    if (frame == nullptr) return;

    if (level < PC_RADIX_LEVELS - 1)
    {
        for (uint32_t i = 0; i < PC_RADIX_FANOUT; i++)
        {
            if (frame->entries[i].u.next_level != nullptr)
                pcRadixCleanup(frame->entries[i].u.next_level, level + 1);
        }
    }

    delete[] frame->entries;
    delete frame;
}

// ============================================================================
// PC hashing helpers
// ============================================================================

uint32_t TemporalPTEPrefetcher::pcHash(IntPtr eip) const
{
    // Simple XOR-fold hash
    uint64_t v = static_cast<uint64_t>(eip);
    v ^= (v >> 16);
    v ^= (v >> 8);
    return static_cast<uint32_t>(v);
}

uint64_t TemporalPTEPrefetcher::pcTag(IntPtr eip) const
{
    // Return the full 64-bit PC as the tag (SRAM cache exact comparison).
    // The radix tree uses only the lower 45 bits (5 levels × 9 bits);
    // upper bits are ignored during tree traversal.
    return static_cast<uint64_t>(eip);
}

// ============================================================================
// Inflight tracking
// ============================================================================
//
// Prefetch lifecycle:
//
//   [prediction issued] ──insertInflight()──→ m_inflight (region_id → metadata)
//                                                │
//                                      TLB materializes page
//                                                │
//                                      ──notifyInstall()──→ m_installed
//                                                │
//                                      demand arrives for region
//                                                │
//                                      ──performPrefetch Phase 0──→ consumed
//
// Both maps are keyed by region_id (vpn >> region_shift) because the
// prediction unit is a region, not a single page.  Multiple pages in
// the same region share one entry.
//
// Metadata stored per entry (InflightEntry):
//   issue_time_ns      — when PTWTransparent was called
//   completion_time_ns — when the PTW walk completed (q.timestamp)
//   source             — TEMPORAL / STRIDE / CHAINED
//   confidence         — slot confidence at prediction time
//   depth              — chain depth (0 = demand-triggered)
//
// This enables rich post-simulation analysis:
//   - Accuracy: installed / (installed + inflight + not_prefetched)
//   - Timeliness: lead time = demand_time - completion_time
//   - Late gap: remaining walk time = completion_time - demand_time
//   - Per-source breakdown: which prediction mechanism is most useful
//   - Per-depth breakdown: are deeper chains accurate?
// ============================================================================

void TemporalPTEPrefetcher::insertInflight(uint64_t region_id, uint64_t issue_time_ns,
                                            uint64_t completion_time_ns,
                                            PrefetchSource source,
                                            uint32_t confidence, uint32_t depth)
{
    auto it = m_inflight.find(region_id);
    if (it != m_inflight.end())
        m_stats.inflight_overwrites++;

    uint64_t walk_lat = (completion_time_ns > issue_time_ns) ? (completion_time_ns - issue_time_ns) : 0;
    m_inflight[region_id] = {issue_time_ns, completion_time_ns, walk_lat, source, confidence, depth};
    m_stats.inflight_inserted++;

    // Per-depth insertion tracking
    if (depth < Stats::MAX_CHAIN_DEPTH)
        m_stats.depth_inflight_inserted[depth]++;

    // High-water mark
    uint64_t sz = static_cast<uint64_t>(m_inflight.size());
    if (sz > m_stats.inflight_high_water_mark)
        m_stats.inflight_high_water_mark = sz;
}

// ============================================================================
// notifyInstall — called by TLB when a prefetch queue entry is installed
// ============================================================================
//
// The TLB calls notifyInstall() when it allocates a PQ entry into the TLB
// proper (the prefetched translation becomes "live").  We move the region
// from m_inflight to m_installed so that Phase 0 in performPrefetch() can
// correctly classify it as an installed hit rather than a late inflight hit.
//
// Since multiple pages in the same region may be installed at different
// times, only the first page triggers the move — subsequent pages in the
// same region find the entry already in m_installed (no-op).
// ============================================================================

void TemporalPTEPrefetcher::notifyInstall(IntPtr address, int page_size)
{
    // Compute region_id matching the plane used at prediction time:
    // 2MB pages use PAGE_SHIFT_2MB (21) + REGION_2MB_TAG; 4KB pages use m_page_shift (12).
    uint64_t rgn;
    if (page_size == 21)
    {
        uint64_t vpn_2mb = static_cast<uint64_t>(address) >> PAGE_SHIFT_2MB;
        rgn = (vpn_2mb >> m_region_shift) | REGION_2MB_TAG;
    }
    else
    {
        uint64_t vpn = static_cast<uint64_t>(address) >> m_page_shift;
        rgn = vpn >> m_region_shift;
    }

    auto it = m_inflight.find(rgn);
    if (it != m_inflight.end())
    {
        // Move to installed map (preserving all metadata for demand-time stats).
        // Multiple pages in the same region share the same entry — the first
        // page to be installed triggers the move; subsequent pages are no-ops
        // (the region is already in m_installed by then).
        if (m_installed.count(rgn))
            m_stats.installed_overwrites++;
        m_installed[rgn] = it->second;
        m_inflight.erase(it);
        m_stats.inflight_installed++;

        uint64_t sz = static_cast<uint64_t>(m_installed.size());
        if (sz > m_stats.installed_high_water_mark)
            m_stats.installed_high_water_mark = sz;
    }
    // If not in inflight: either this region was already moved to m_installed
    // by a sibling page, or it belongs to a different prefetcher — nothing to do.
}

// ═══════════════════════════════════════════════════════════════════
//  In-PTE payload writeback modeling
//
//  Called by the MMU AFTER the demand PTW completes (so PTE lines just
//  walked are resident).  For each in-PTE payload (delta) updated during
//  this access's learning, locate the source PTE's physical cacheline via
//  a functional walk and mark it dirty (dirty-if-present).  The cache's
//  existing eviction path then generates the DRAM writeback, so the extra
//  traffic the reviewer asked about is modeled end-to-end.
// ═══════════════════════════════════════════════════════════════════
void TemporalPTEPrefetcher::flushPendingPayloadWritebacks(PageTable *pt)
{
    if (!m_model_payload_writeback || m_pending_payload_wb.empty() || pt == nullptr)
    {
        m_pending_payload_wb.clear();
        return;
    }

    // PTW (PAGE_TABLE_DATA) lines bypass L1D and reside in L2; that is where
    // the just-walked PTE line lives.
    MMUCacheInterface *l2 = memory_manager->getCacheCntlrAt(core->getId(), MemComponent::L2_CACHE);
    if (l2 == nullptr)
    {
        m_pending_payload_wb.clear();
        return;
    }

    for (const auto &upd : m_pending_payload_wb)
    {
        const uint64_t vpn    = upd.first;
        const bool     is_2mb = upd.second;
        const IntPtr   addr   = is_2mb ? (static_cast<IntPtr>(vpn) << PAGE_SHIFT_2MB)
                                       : (static_cast<IntPtr>(vpn) << m_page_shift);

        // Functional walk (no timing/count) to find the leaf/PMD PTE's PA.
        PTWResult r = pt->initializeWalk(addr, /*count*/ false, /*is_prefetch*/ true, /*restart_walk*/ true);
        if (r.fault_happened)
            continue;

        IntPtr pte_pa = 0;
        for (const auto &a : r.accesses)
            if (a.is_pte) { pte_pa = a.physical_addr; break; }
        if (pte_pa == 0 && !r.accesses.empty())
            pte_pa = r.accesses.back().physical_addr;   // fallback: deepest level
        if (pte_pa == 0)
            continue;

        m_stats.payload_writes++;
        switch (l2->markMMUPayloadDirty(pte_pa))
        {
        case 1:  m_stats.payload_dirty_marks_hit++;        break;  // clean -> dirty (new writeback)
        case 2:  m_stats.payload_dirty_marks_coalesced++;  break;  // already dirty (coalesced)
        default: m_stats.payload_dirty_marks_miss++;       break;  // not resident
        }
    }
    m_pending_payload_wb.clear();
}

} // namespace ParametricDramDirectoryMSI
