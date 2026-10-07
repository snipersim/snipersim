#pragma once

/**
 * NUMA-Aware Revelator Speculative Engine
 * ========================================
 *
 * Combines three mechanisms for NUMA-scoped address prediction:
 *
 * 1. **PTW-driven Saturating Counters** (primary, fast path)
 *    A 2-D array [num_counter_entries][num_numa_nodes] of saturating
 *    counters indexed by hash(VPN).  On each PTW resolution the counter
 *    for the *actual* NUMA node is incremented and counters for other
 *    nodes are decremented.  Confidence = gap between top-1 and top-2.
 *
 * 2. **NUMA Hint Table** (fallback, when counters are not confident)
 *    A sparse radix tree mapping VPN → {valid, NUMA node id (2 bits)}.
 *    Layout identical to RadixWayTable (Utopia) but leaves store
 *    [valid:1][node_id:2] = 3 bits per entry.  Updated on every PTW
 *    resolution via allocateInSpecEngine().
 *    Only consulted when counters drift (migration, remote access, phase
 *    change) — this reduces hint table traffic during steady state.
 *
 * 3. **Revelator Hash Predictions**
 *    Per-candidate-node hash → PFN, reusing the standard Revelator
 *    hashing scheme.  The candidate set is bounded by policy + confidence.
 *
 * HYBRID CONFIDENCE-DRIVEN NODE SELECTION (selectCandidateNodes):
 *   0) Check counter confidence first (fast path)
 *   1) Include policy-required "primary" node (local / preferred / interleave)
 *   2) If confident: use counter top-K only (skip hint table)
 *   3) If not confident: consult hint table, then add counter backup
 *   4) Last-resort: cpu-local fallback (for local/preferred policies)
 *   Short-circuit: interleaved policy → deterministic node, no hints/counters
 *
 * POLICY-AWARE NODE SELECTION:
 *   - LOCAL:       primary = cpu_local_node, cpu-local fallback = yes
 *   - PREFERRED:   primary = preferred_node (config), cpu-local fallback = yes
 *   - INTERLEAVED: primary = VPN % interleave_mask, fanout = 1, no hints
 *
 * TRAINING FLOW (allocateInSpecEngine — called after PTW resolves PPN):
 *   a) Determine NUMA node from PPN using per-node PFN ranges
 *   b) Set hint in NumaHintTable for this VPN → actual node (shadow learning)
 *   c) Update counter[hash(vpn)][actual_node] += delta_inc
 *      Update counter[hash(vpn)][other_node]  -= delta_dec
 *   Both structures are always updated (shadow learning), even when only
 *   counters drive node selection — so hints stay fresh for when counters drift.
 *
 * PFN GEOMETRY (aligned with allocator):
 *   global_pfn = global_kernel_pages + node.start_index + hash_within_usable
 *   per-node range is usable-only (kernel pages are a global prefix)
 *
 * Config: perf_model/dram/numa/spec_engine_type = "numa_revelator"
 */

#include "mmu.h"
#include "mmu_base.h"
#include "config.hpp"
#include "spec_engine_base.h"
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "shmem_perf_model.h"
#include "pagetable.h"
#include "cache_block_info.h"
#include "stats.h"
#include "city.h"
#include "memory_management/numa/numa_hint_table.h"

#include <algorithm>
#include <fstream>
#include <vector>
#include <cstdint>
#include <mutex>

namespace ParametricDramDirectoryMSI
{

    // ====================================================================
    // PTW-driven saturating counters   [counter_entries × num_nodes]
    // ====================================================================
    class NumaNodeCounters
    {
    public:
        static constexpr int SAT_MAX = 31;   // 5-bit saturating counter
        static constexpr int SAT_MIN = 0;

    private:
        UInt32 m_num_entries;   // number of hash buckets
        UInt32 m_num_nodes;
        int    m_delta_inc;     // increment on PTW match
        int    m_delta_dec;     // decrement on PTW mismatch
        int    m_confidence_threshold;  // gap for confidence

        // 2-D array: m_counters[entry_idx * m_num_nodes + node]
        std::vector<int> m_counters;
        mutable std::mutex m_lock;

        // stats
        UInt64 m_updates        = 0;
        UInt64 m_confident_hits = 0;

    public:
        NumaNodeCounters(UInt32 num_entries = 4096, UInt32 num_nodes = 4,
                         int delta_inc = 2, int delta_dec = 1,
                         int confidence_threshold = 4)
            : m_num_entries(num_entries)
            , m_num_nodes(num_nodes)
            , m_delta_inc(delta_inc)
            , m_delta_dec(delta_dec)
            , m_confidence_threshold(confidence_threshold)
        {
            m_counters.assign(m_num_entries * m_num_nodes, 0);
        }

        /// Hash VPN to a counter-table index
        UInt32 hashIndex(UInt64 vpn) const {
            return CityHash64((const char*)&vpn, sizeof(vpn)) % m_num_entries;
        }

        /// Called after PTW resolves VPN→PPN (actual NUMA node known)
        void update(UInt64 vpn, UInt32 actual_node) {
            std::lock_guard<std::mutex> guard(m_lock);
            m_updates++;
            UInt32 idx = hashIndex(vpn);
            int base   = idx * m_num_nodes;
            for (UInt32 n = 0; n < m_num_nodes; ++n) {
                if (n == actual_node) {
                    m_counters[base + n] = std::min(m_counters[base + n] + m_delta_inc, SAT_MAX);
                } else {
                    m_counters[base + n] = std::max(m_counters[base + n] - m_delta_dec, SAT_MIN);
                }
            }
        }

        /// Return the top-K node IDs sorted by counter value (descending)
        std::vector<UInt32> getTopKNodes(UInt64 vpn, UInt32 k) const {
            std::lock_guard<std::mutex> guard(m_lock);
            UInt32 idx = hashIndex(vpn);
            int base   = idx * m_num_nodes;

            // Build (score, node) pairs
            std::vector<std::pair<int, UInt32>> scored;
            scored.reserve(m_num_nodes);
            for (UInt32 n = 0; n < m_num_nodes; ++n)
                scored.emplace_back(m_counters[base + n], n);
            std::sort(scored.begin(), scored.end(),
                      [](auto& a, auto& b){ return a.first > b.first; });

            std::vector<UInt32> result;
            for (UInt32 i = 0; i < std::min(k, m_num_nodes); ++i)
                result.push_back(scored[i].second);
            return result;
        }

        /// Check if counters are confident (gap between #1 and #2 exceeds threshold)
        bool isConfident(UInt64 vpn) const {
            std::lock_guard<std::mutex> guard(m_lock);
            UInt32 idx = hashIndex(vpn);
            int base   = idx * m_num_nodes;
            if (m_num_nodes < 2) return true;

            int top1 = SAT_MIN, top2 = SAT_MIN;
            for (UInt32 n = 0; n < m_num_nodes; ++n) {
                int v = m_counters[base + n];
                if (v >= top1) { top2 = top1; top1 = v; }
                else if (v > top2) { top2 = v; }
            }
            return (top1 - top2) >= m_confidence_threshold;
        }

        int getScore(UInt64 vpn, UInt32 node) const {
            std::lock_guard<std::mutex> guard(m_lock);
            UInt32 idx = hashIndex(vpn);
            return m_counters[idx * m_num_nodes + node];
        }

        UInt64 getUpdates()       const { return m_updates; }
        UInt64 getConfidentHits() const { return m_confident_hits; }
        void   incConfidentHits()       { m_confident_hits++; }
    };

    // ====================================================================
    // NumaRevelator spec engine
    // ====================================================================
    class NumaRevelator : public SpecEngineBase
    {
    protected:
        String name;
        MemoryManagerBase *memory_manager;

        // --- base Revelator config ---
        bool oracle_revelator;
        int  number_of_hashes;
        int  number_of_predictions;
        int  rev_type;           // 0=full, 1=data only, 2=translation only
        UInt64 m_memory_size;    // MB
        UInt64 kernel_size;      // MB
        UInt64 m_total_pages;
        UInt64 m_kernel_pages;
        bool has_filter;
        bool perfect_filtering;

        // --- NUMA geometry (aligned with allocator model) ---
        UInt32 m_num_numa_nodes;
        UInt64 m_global_kernel_pages;                  ///< Sum of all per-node kernel pages (global prefix)
        std::vector<UInt64> m_per_node_usable_pages;  ///< Usable pages per node (excl. kernel)
        std::vector<UInt64> m_per_node_start_index;   ///< Start index in flat usable array
        std::vector<UInt64> m_per_node_end_index;     ///< End index (exclusive) in flat usable array
        UInt32 m_max_speculation_nodes;

        // --- NUMA placement policy ---
        String m_numa_policy;                         ///< "local", "preferred", "interleaved"
        UInt32 m_preferred_node;                      ///< For "preferred" policy
        std::vector<UInt32> m_interleave_mask;        ///< Node sequence for interleaved policy

        // --- hint table + counters ---
        NumaHintTable   *m_hint_table;
        NumaNodeCounters *m_counters;

        // --- Stats ---
        UInt64 *hits_per_hash;
        UInt64 *prefetches_per_hash;
        UInt64 *hits_per_hash_pt;
        UInt64 *prefetches_per_hash_pt;
        UInt64 *filtered_predictions_per_hash;
        UInt64 relevator_prefetches;

        // Per-node stats
        UInt64 *per_node_predictions;
        UInt64 *per_node_hits;

        // Hint / counter stats
        UInt64 m_hint_lookups        = 0;
        UInt64 m_hint_hits           = 0;
        UInt64 m_counter_confident   = 0;
        UInt64 m_counter_not_confident = 0;
        UInt64 m_train_updates       = 0;
        UInt64 m_train_node_mismatch = 0;  ///< hint existed but was wrong

        std::string log_file_name;
        std::ofstream log_file;

    public:
        NumaRevelator(Core *core, MemoryManagerBase *_memory_manager,
                      ShmemPerfModel *shmem_perf_model, String _name);

        ~NumaRevelator();

        // ---- SpecEngineBase interface ------------------------------------
        void invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock,
                              IntPtr eip, bool modeled, SubsecondTime invoke_start_time,
                              IntPtr physical_address, bool page_table_speculation = false);

        void allocateInSpecEngine(IntPtr address, IntPtr ppn, int count,
                                  Core::lock_signal_t lock, IntPtr eip, bool modeled);

        // ---- Helpers -----------------------------------------------------
        /// Standard Revelator-style hash into a [0, table_size) range
        UInt64 hashFunction(IntPtr address, int table_size);

        /// Per-node hash (same as hashFunction but with node-scoped table_size)
        UInt64 nodeHashFunction(IntPtr address, UInt64 node_table_size);

        /// Determine NUMA node from PPN using per-node PFN ranges. Returns m_num_numa_nodes on failure.
        UInt32 ppnToNumaNode(IntPtr ppn) const;

        struct NumaPrediction {
            IntPtr predicted_address;
            UInt32 node_id;
            int    hash_index;
        };

        /// Generate NUMA-scoped predictions for @p address
        std::vector<NumaPrediction> predictNuma(IntPtr address, int num_predictions,
                                                 bool is_page_table, UInt32 cpu_local_node = 0);

        /// Hybrid confidence-driven candidate node selection
        std::vector<UInt32> selectCandidateNodes(UInt64 vpn, UInt32 cpu_local_node);

        /// Policy-aware primary node (local / preferred / interleaved)
        UInt32 getPrimaryNodeForPolicy(UInt64 vpn, UInt32 cpu_local_node) const;

        /// Whether to force CPU-local as last-resort fallback (not for interleaved)
        bool shouldForceCpuLocalFallback() const;

        /// Legacy fallback: predict for node 0 only
        std::vector<IntPtr> predict(IntPtr address, int num_predictions,
                                    bool is_page_table = false);
    };

}
