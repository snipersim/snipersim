#pragma once

/**
 * NUMA-aware Reservation-based THP Allocator
 * 
 * Extends the ReservationTHPAllocator to NUMA systems by maintaining 
 * one NumaBuddyAllocator (with per-node buddies) and per-node 2MB maps.
 *
 * Key design decisions:
 *   - Each NUMA node has its own buddy allocator for physical memory
 *   - 2MB reservations are node-local: a reservation stays on a single node
 *   - NUMA policy (LOCAL/BIND/INTERLEAVE) determines which node gets the allocation
 *   - Utilization-based spill: when a node exceeds threshold, spill to others
 *   - THP promotion considers per-node utilization (avoid cross-node huge pages)
 *
 * NUMA Policies:
 *   - LOCAL: Reserve 2MB on CPU-local node; promotes there. Excellent when 
 *            block-level fragmentation is low. Sensitive to per-node fragmentation.
 *   - BIND:  Reserve 2MB on bound nodes. Possibility of allocating large pages
 *            across more than one node.
 *   - INTERLEAVE: New hash function maps 2MB VPN to a NUMA node, then reserves
 *                 within that node. Structured contiguity works with interleaving
 *                 via 2MB-page-to-node mapping.
 */

#include "debug_config.h"
#include "memory_management/physical_memory_allocators/physical_memory_allocator.h"
#include "memory_management/numa/numa_policy.h"
#include "memory_management/numa/numa_buddy_allocator.h"
#include "simulator.h"
#include "config.hpp"
#include <bitset>
#include <map>
#include <tuple>
#include <vector>
#include <utility>
#include <cassert>

#include "templates_traits_config.h"
#include "memory_management/physical_memory_allocators/buddy_policy_traits.h"
#include "memory_management/physical_memory_allocators/buddy.h"

#include "debug_config.h"

enum class NumaFragmentationMode {
    RATIO_BASED,
    COUNT_BASED
};

template <typename Policy>
class NumaReservationTHPAllocator : public PhysicalMemoryAllocator, private Policy
{
    using BuddyPolicy = typename BuddyPolicyFor<Policy>::type;
    using BuddyType = Buddy<BuddyPolicy>;
    using NumaBuddyType = NumaBuddyAllocator<BuddyPolicy>;

private:
    struct Stats {
        UInt64 four_kb_allocated = 0;
        UInt64 two_mb_reserved = 0;
        UInt64 two_mb_promoted = 0;
        UInt64 two_mb_demoted = 0;
        UInt64 total_allocations = 0;
        UInt64 kernel_pages_used = 0;
        // NUMA-specific stats
        UInt64* per_node_allocs = nullptr;
        UInt64* per_node_2mb_reserved = nullptr;
        UInt64* per_node_2mb_promoted = nullptr;
        UInt64* per_node_spills = nullptr;
        UInt64 local_allocs = 0;
        UInt64 bind_allocs = 0;
        UInt64 interleave_allocs = 0;
    } stats;

    NumaFragmentationMode m_frag_mode = NumaFragmentationMode::RATIO_BASED;
    double m_target_frag_ratio = 1.0;
    UInt64 m_target_free_2mb_count = 0;

    double threshold_for_promotion;

    // NUMA state
    UInt32 m_num_numa_nodes;
    NumaPlacementEngine m_placement_engine;
    NumaBuddyType* m_numa_buddy;

    // Per-node 2MB reservation maps
    // node_id -> { 2MB_region_index -> (start_PFN, used_bitmap, promoted?) }
    std::vector<std::map<UInt64, std::tuple<UInt64, std::bitset<512>, bool>>> m_per_node_2mb_maps;

    // Per-node kernel bump pointers (in PFN units)
    // Each node's kernel region is [base_pfn, base_pfn + kernel_pages).
    // Node 0 starts at PFN 1 (PFN 0 is reserved for root page table).
    std::vector<UInt64> m_per_node_kernel_next_pfn;
    std::vector<UInt64> m_per_node_kernel_end_pfn;

public:
    Stats& getStats() { return stats; }

    NumaReservationTHPAllocator(String name,
                                 int memory_size,
                                 int max_order,
                                 int kernel_size,
                                 String frag_type,
                                 double threshold_for_promotion,
                                 // NUMA params
                                 UInt32 num_numa_nodes,
                                 NumaPolicy numa_policy,
                                 double utilization_threshold,
                                 const std::vector<UInt64>& per_node_capacity_mb,
                                 const std::vector<UInt64>& per_node_kernel_mb)
        : PhysicalMemoryAllocator(name, memory_size, kernel_size)
        , threshold_for_promotion(threshold_for_promotion)
        , m_num_numa_nodes(num_numa_nodes)
    {
        // The per-node statistics are allocated before the policy runs, since
        // on_init registers them (and only registers what exists).
        stats.per_node_allocs = (UInt64*)calloc(num_numa_nodes, sizeof(UInt64));
        stats.per_node_2mb_reserved = (UInt64*)calloc(num_numa_nodes, sizeof(UInt64));
        stats.per_node_2mb_promoted = (UInt64*)calloc(num_numa_nodes, sizeof(UInt64));
        stats.per_node_spills = (UInt64*)calloc(num_numa_nodes, sizeof(UInt64));

        // Initialize policy
        Policy::on_init(name, memory_size, kernel_size, threshold_for_promotion, this);

        static_assert(is_complete<BuddyPolicyFor<Policy>>::value,
            "BuddyPolicyFor<Policy> is incomplete. Did you include the correct buddy_policy.h?");

        // === Initialize NUMA placement engine ===
        m_placement_engine.init(num_numa_nodes, numa_policy,
                                 1,  // num_hashes (THP doesn't use multi-hash)
                                 utilization_threshold);

        // === Create NUMA buddy allocator ===
        m_numa_buddy = new NumaBuddyType(
            num_numa_nodes,
            per_node_capacity_mb,
            per_node_kernel_mb,
            max_order,
            frag_type,
            0 // global kernel offset handled differently
        );

        // Configure placement engine nodes
        for (UInt32 i = 0; i < num_numa_nodes; ++i)
        {
            UInt64 node_cap_mb = (i < per_node_capacity_mb.size()) ? per_node_capacity_mb[i] : memory_size / num_numa_nodes;
            UInt64 node_kern_mb = (i < per_node_kernel_mb.size()) ? per_node_kernel_mb[i] : kernel_size / num_numa_nodes;
            UInt64 usable_bytes = (node_cap_mb - node_kern_mb) * 1024ULL * 1024;

            m_placement_engine.configureNode(
                i, usable_bytes,
                node_kern_mb * 1024ULL * 1024,
                m_numa_buddy->getNodeBasePFN(i),
                m_numa_buddy->getNodeBasePFN(i) + m_numa_buddy->getNodeNumPages(i),
                "ddr"
            );
        }

        // Initialize per-node 2MB maps
        m_per_node_2mb_maps.resize(num_numa_nodes);

        // Initialize per-node kernel bump pointers
        // Each node's kernel region occupies PFNs [base_pfn, base_pfn + kernel_pages).
        // Node 0: skip PFN 0 (reserved for root page table), start at PFN 1.
        m_per_node_kernel_next_pfn.resize(num_numa_nodes);
        m_per_node_kernel_end_pfn.resize(num_numa_nodes);
        for (UInt32 i = 0; i < num_numa_nodes; ++i)
        {
            UInt64 base = m_numa_buddy->getNodeBasePFN(i);
            UInt64 kpages = m_numa_buddy->getNodeKernelPages(i);
            m_per_node_kernel_next_pfn[i] = (i == 0) ? base + 1 : base; // skip PFN 0 on node 0
            m_per_node_kernel_end_pfn[i] = base + kpages;
            std::cout << "[NumaReserveTHP] Node " << i
                      << " kernel PFN range [" << m_per_node_kernel_next_pfn[i]
                      << ", " << m_per_node_kernel_end_pfn[i] << ")" << std::endl;
        }
    }

    ~NumaReservationTHPAllocator()
    {
        delete m_numa_buddy;
        free(stats.per_node_allocs);
        free(stats.per_node_2mb_reserved);
        free(stats.per_node_2mb_promoted);
        free(stats.per_node_spills);
    }

    // =========================================================================
    // NUMA-aware allocation
    // =========================================================================

    std::pair<UInt64, UInt64> allocate(UInt64 size, UInt64 address = 0,
                                       UInt64 core_id = -1,
                                       bool is_pagetable_allocation = false,
                                       bool is_instruction_allocation = false) override
    {
        stats.total_allocations++;
        this->log("numa_allocate: size=", size, "addr=", address, "core=", core_id);

        // === Determine CPU-local NUMA node from core_id ===
        UInt32 cpu_local_node = 0;
        if (m_num_numa_nodes > 1 && core_id != static_cast<UInt64>(-1))
        {
            UInt32 cores_total = Sim()->getConfig()->getApplicationCores();
            UInt32 cores_per_node = (cores_total > 0 && m_num_numa_nodes > 0)
                                     ? cores_total / m_num_numa_nodes : 1;
            if (cores_per_node == 0) cores_per_node = 1;
            cpu_local_node = static_cast<UInt32>(core_id) / cores_per_node;
            if (cpu_local_node >= m_num_numa_nodes)
                cpu_local_node = m_num_numa_nodes - 1;
        }

        // NOTE: Page table allocations go through handle_page_table_allocations() override,
        // not through this method. The is_pagetable_allocation flag is unused here.

        // === Determine target NUMA node for data allocation ===
        UInt64 vpn = address >> 12;
        UInt32 target_node = m_placement_engine.selectAllocationNode(vpn, cpu_local_node);

        // === Try 2MB allocation on target node ===
        auto page2mb = checkFor2MBAllocationOnNode(address, core_id, target_node);
        this->log("NUMA 2MB check on node", target_node, "result=", page2mb.first);

        if (page2mb.first != static_cast<UInt64>(-1))
        {
            stats.per_node_allocs[target_node]++;
            trackPolicyAlloc();
            m_placement_engine.setNodeHint(vpn, target_node);
            m_placement_engine.recordAllocation(target_node, 4096);

            if (page2mb.second) {
                return std::make_pair(page2mb.first, 21);
            } else {
                return std::make_pair(page2mb.first, 12);
            }
        }

        // === Fallback: buddy 4KB on target node ===
        auto fallback = m_numa_buddy->allocate(target_node, size, address, core_id);
        if (fallback != static_cast<UInt64>(-1))
        {
            stats.four_kb_allocated++;
            stats.per_node_allocs[target_node]++;
            trackPolicyAlloc();
            m_placement_engine.setNodeHint(vpn, target_node);
            m_placement_engine.recordAllocation(target_node, 4096);
            return std::make_pair(fallback, 12);
        }

        // === Try demoting a 2MB region on this node ===
        if (demote_page_on_node(target_node))
        {
            auto retried = m_numa_buddy->allocate(target_node, size, address, core_id);
            if (retried != static_cast<UInt64>(-1))
            {
                stats.four_kb_allocated++;
                stats.per_node_allocs[target_node]++;
                m_placement_engine.setNodeHint(vpn, target_node);
                m_placement_engine.recordAllocation(target_node, 4096);
                return std::make_pair(retried, 12);
            }
        }

        // === Spill: try other nodes ===
        for (UInt32 n = 0; n < m_num_numa_nodes; ++n)
        {
            if (n == target_node) continue;

            // Try 2MB on spill node
            auto spill_2mb = checkFor2MBAllocationOnNode(address, core_id, n);
            if (spill_2mb.first != static_cast<UInt64>(-1))
            {
                stats.per_node_allocs[n]++;
                stats.per_node_spills[n]++;
                m_placement_engine.setNodeHint(vpn, n);
                m_placement_engine.recordAllocation(n, 4096);
                return std::make_pair(spill_2mb.first, spill_2mb.second ? 21 : 12);
            }

            // Try buddy 4KB on spill node
            auto spill_4kb = m_numa_buddy->allocate(n, size, address, core_id);
            if (spill_4kb != static_cast<UInt64>(-1))
            {
                stats.four_kb_allocated++;
                stats.per_node_allocs[n]++;
                stats.per_node_spills[n]++;
                m_placement_engine.setNodeHint(vpn, n);
                m_placement_engine.recordAllocation(n, 4096);
                return std::make_pair(spill_4kb, 12);
            }
        }

        // Out of memory
        assert(false && "NUMA ReserveTHP: out of memory on all nodes");
        return std::make_pair((UInt64)-1, 12);
    }

    std::vector<Range> allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id) override
    {
        return {};
    }

    void fragment_memory(double target_fragmentation) override
    {
        if (m_frag_mode == NumaFragmentationMode::COUNT_BASED) {
            for (UInt32 i = 0; i < m_num_numa_nodes; ++i)
                m_numa_buddy->fragmentMemoryToTargetCount(i, m_target_free_2mb_count / m_num_numa_nodes);
        } else {
            m_numa_buddy->fragmentAllNodes(target_fragmentation);
        }
    }

    void fragment_memory_to_count(UInt64 target_free_2mb_pages) override
    {
        // Distribute target count proportionally across nodes
        for (UInt32 i = 0; i < m_num_numa_nodes; ++i)
        {
            UInt64 per_node_target = target_free_2mb_pages / m_num_numa_nodes;
            m_numa_buddy->fragmentMemoryToTargetCount(i, per_node_target);
        }
    }

    UInt64 givePageFast(UInt64 bytes, UInt64 address = 0, UInt64 core_id = -1) override
    {
        // Fast path: allocate on node 0
        auto result = m_numa_buddy->allocate(0, bytes, address, core_id);
        if (result == static_cast<UInt64>(-1))
            result = m_numa_buddy->allocateAny(bytes, address, core_id);
        return result;
    }

    void deallocate(UInt64 region, UInt64 core_id = -1) override
    {
        // Determine which node this PFN belongs to
        UInt32 node_id = m_numa_buddy->getNodeForPFN(region);
        m_placement_engine.recordDeallocation(node_id, 4096);
        // TODO: proper deallocation from buddy and 2MB maps
    }

    // =========================================================================
    // NUMA-aware page table allocation (kernel pool)
    // =========================================================================

    /**
     * Override base class to allocate page table pages from the
     * CPU-local NUMA node's kernel region instead of a single global pool.
     */
    UInt64 handle_page_table_allocations(UInt64 bytes, UInt64 core_id = (UInt64)-1) override
    {
        UInt32 target_node = 0;
        if (m_num_numa_nodes > 1 && core_id != static_cast<UInt64>(-1))
        {
            UInt32 cores_total = Sim()->getConfig()->getApplicationCores();
            UInt32 cores_per_node = (cores_total > 0 && m_num_numa_nodes > 0)
                                     ? cores_total / m_num_numa_nodes : 1;
            if (cores_per_node == 0) cores_per_node = 1;
            target_node = static_cast<UInt32>(core_id) / cores_per_node;
            if (target_node >= m_num_numa_nodes)
                target_node = m_num_numa_nodes - 1;
        }

        UInt64 pages_needed = (bytes + 4095) / 4096; // round up to pages
        UInt64 pfn = m_per_node_kernel_next_pfn[target_node];

        if (pfn + pages_needed > m_per_node_kernel_end_pfn[target_node])
        {
            // Node's kernel pool exhausted — spill to any other node
            for (UInt32 n = 0; n < m_num_numa_nodes; ++n)
            {
                if (n == target_node) continue;
                if (m_per_node_kernel_next_pfn[n] + pages_needed <= m_per_node_kernel_end_pfn[n])
                {
                    pfn = m_per_node_kernel_next_pfn[n];
                    m_per_node_kernel_next_pfn[n] += pages_needed;
                    stats.per_node_spills[n]++;
                    return pfn;
                }
            }
            assert(false && "NUMA ReserveTHP: out of kernel memory on all nodes for page tables");
            return static_cast<UInt64>(-1);
        }

        m_per_node_kernel_next_pfn[target_node] += pages_needed;
        stats.kernel_pages_used += pages_needed;
        return pfn;
    }

    void handle_page_table_deallocations(UInt64 bytes) override
    {
        // For the NUMA bump allocator we don't reclaim per-node kernel pages.
        // The base class version simply decrements the global pointer, which
        // is meaningless here.  A proper free-list could be added later.
    }

    // =========================================================================
    // NUMA accessors
    // =========================================================================

    NumaPlacementEngine& getPlacementEngine() { return m_placement_engine; }
    UInt32 getNumNumaNodes() const { return m_num_numa_nodes; }
    NumaBuddyType* getNumaBuddy() { return m_numa_buddy; }

    /**
     * @brief Authoritative PPN → NUMA node lookup.
     *
     * Delegates to the underlying NumaBuddyAllocator which owns the per-node
     * PFN ranges.  This is the canonical source of truth for which physical
     * page belongs to which NUMA node.
     */
    UInt32 getNumaNodeForPPN(UInt64 ppn) const override
    {
        return m_numa_buddy->getNodeForPFN(ppn);
    }

    IntPtr isLargePageReserved(IntPtr address) {
        UInt64 region_2MB = address >> 21;
        for (UInt32 n = 0; n < m_num_numa_nodes; ++n)
        {
            auto& map = m_per_node_2mb_maps[n];
            if (map.find(region_2MB) != map.end())
                return std::get<0>(map[region_2MB]);
        }
        return -1;
    }

    // Fragmentation mode setters
    void setFragmentationMode(NumaFragmentationMode mode) { m_frag_mode = mode; }
    void setTargetFree2MBCount(UInt64 count) { m_target_free_2mb_count = count; }

    UInt64 getCurrentFree2MBCount() const
    {
        UInt64 total = 0;
        for (UInt32 i = 0; i < m_num_numa_nodes; ++i)
            total += m_numa_buddy->getNodeFreeLargePages(i);
        return total;
    }

    UInt64 getCurrentFree2MBCountOnNode(UInt32 node_id) const
    {
        return m_numa_buddy->getNodeFreeLargePages(node_id);
    }

protected:

    // =========================================================================
    // Per-node 2MB reservation logic
    // =========================================================================

    /**
     * Try to allocate a 4KB page within a 2MB reservation on a specific NUMA node.
     * Same logic as the non-NUMA version, but uses per-node buddy and maps.
     */
    std::pair<UInt64, bool> checkFor2MBAllocationOnNode(UInt64 address, UInt64 core_id,
                                                         UInt32 node_id)
    {
        if (node_id >= m_num_numa_nodes)
            return std::make_pair((UInt64)-1, false);

        UInt64 region_2MB = address >> 21;
        int offset_in_2MB = (address >> 12) & 0x1FF;
        auto& two_mb_map = m_per_node_2mb_maps[node_id];

        // If region not yet reserved on this node, try reserving
        if (two_mb_map.find(region_2MB) == two_mb_map.end())
        {
            auto reserved = m_numa_buddy->reserve2MBPage(node_id, address, core_id);

            if (std::get<0>(reserved) == static_cast<UInt64>(-1))
                return std::make_pair((UInt64)-1, false);

            two_mb_map[region_2MB] = std::make_tuple(
                std::get<0>(reserved), std::bitset<512>(), false);
            stats.two_mb_reserved++;
            stats.per_node_2mb_reserved[node_id]++;
        }

        auto& region = two_mb_map[region_2MB];

        if (std::get<2>(region))
        {
            // Already promoted — shouldn't be called again
            assert(false);
        }

        auto& bitset = std::get<1>(region);
        bitset.set(offset_in_2MB);

        float utilization = static_cast<float>(bitset.count()) / 512;
        bool ready_to_promote = (utilization > threshold_for_promotion);

        if (ready_to_promote && !std::get<2>(region))
        {
            std::get<2>(region) = true;
            stats.two_mb_promoted++;
            stats.per_node_2mb_promoted[node_id]++;
            return std::make_pair(std::get<0>(region), true);
        }
        else
        {
            return std::make_pair(std::get<0>(region) + offset_in_2MB, false);
        }
    }

    /**
     * Demote least-utilized 2MB region on a specific NUMA node.
     */
    bool demote_page_on_node(UInt32 node_id)
    {
        if (node_id >= m_num_numa_nodes)
            return false;

        auto& two_mb_map = m_per_node_2mb_maps[node_id];
        std::vector<std::pair<UInt64, double>> utilization;

        for (auto it = two_mb_map.begin(); it != two_mb_map.end(); ++it)
        {
            if (std::get<2>(it->second)) continue; // skip promoted
            double util = static_cast<double>(std::get<1>(it->second).count()) / 512;
            utilization.push_back(std::make_pair(it->first, util));
        }

        std::sort(utilization.begin(), utilization.end(),
                  [](auto& l, auto& r) { return l.second < r.second; });

        if (utilization.empty())
            return false;

        UInt64 region_2MB = utilization[0].first;
        UInt64 region_begin = std::get<0>(two_mb_map[region_2MB]);

        for (int j = 0; j < 512; j++)
        {
            if (std::get<1>(two_mb_map[region_2MB])[j])
                continue; // allocated page, don't free
            m_numa_buddy->free(region_begin + j, region_begin + j + 1);
        }

        stats.two_mb_demoted++;
        two_mb_map.erase(region_2MB);
        return true;
    }

    /**
     * Demote across any node (global fallback).
     */
    bool demote_page()
    {
        for (UInt32 n = 0; n < m_num_numa_nodes; ++n)
            if (demote_page_on_node(n))
                return true;
        return false;
    }

private:
    void trackPolicyAlloc()
    {
        NumaPolicy p = m_placement_engine.getPolicy();
        if (p == NumaPolicy::LOCAL || p == NumaPolicy::PREFERRED)
            stats.local_allocs++;
        else if (p == NumaPolicy::BIND)
            stats.bind_allocs++;
        else if (p == NumaPolicy::INTERLEAVE)
            stats.interleave_allocs++;
    }
};
