#pragma once

/**
 * NUMA-aware Revelator Allocator
 * 
 * Extends the hash-based Revelator allocation scheme to NUMA systems.
 * 
 * Key design decisions (from paper):
 *   - Node-scoped hashing: H_i computes offsets only within the PFN ranges
 *     of the selected NUMA node, ensuring policy correctness and bounded fanout.
 *   - Policy-first allocation: OS placement policy defines allowed node set,
 *     then Revelator hashes within that node's PFN range.
 *   - Graceful fallback: if hash-based allocation fails on target node(s),
 *     falls back to linear scan within the node, then spills to other nodes.
 *
 * NUMA Policies:
 *   - LOCAL:       Hash within CPU-local node; spill to others under pressure
 *   - BIND:        Hash within bound nodemask; strict allocation
 *   - INTERLEAVE:  Deterministic VPN→Node-ID mapping, hash within target node
 *                  Requires NUMA-aware hash function for policy correctness
 *
 * Speculation:
 *   - For LOCAL/PREFERRED: counters + hints determine candidate nodes
 *   - For INTERLEAVE: deterministic node from VPN → low fanout
 *   - For BIND: speculate across bound nodes, filter by utilization
 */

#include "physical_memory_allocator.h"
#include "memory_management/numa/numa_policy.h"
#include <vector>
#include <fstream>
#include <city.h>
#include <cstdlib>

template <typename Policy>
class NumaRevelatorAllocator : public PhysicalMemoryAllocator, private Policy
{
public:
    struct HashStats {
        UInt64* hits;
        UInt64* tries;
        UInt64* swap_attempts;
        UInt64* swap_attempts_same_address;
        UInt64* successful_pt_allocations;
    };

    struct AllocationEntry {
        IntPtr   address;
        bool     is_free;
        bool     is_poisoned;
        bool     is_pagetable_allocation;
        UInt64   app_id;
        UInt32   node_id;   // NUMA node this entry resides on
    };

    // Per-NUMA-node state
    struct NodeState {
        UInt32 node_id;
        UInt64 start_index;    // Start index in memory_allocations for this node
        UInt64 num_pages;      // Number of usable pages on this node
        UInt64 kernel_pages;   // Kernel-reserved pages on this node
        UInt64 pages_used;     // Currently allocated pages on this node

        // Per-node allocation stats
        UInt64 alloc_count;
        UInt64 hash_hits;
        UInt64 hash_misses;
        UInt64 fallback_allocs;

        NodeState() : node_id(0), start_index(0), num_pages(0), kernel_pages(0),
                       pages_used(0), alloc_count(0), hash_hits(0), hash_misses(0),
                       fallback_allocs(0) {}

        double utilization() const {
            if (num_pages == 0) return 0.0;
            return static_cast<double>(pages_used) / num_pages;
        }
    };

    NumaRevelatorAllocator(String name,
                            int max_order,
                            UInt64 kernel_size,
                            String frag_type,
                            int m_number_of_hashes,
                            UInt64 memory_size,
                            double _target_frag,
                            bool _enable_aggressive_swapouts,
                            int _infrequency_threshold,
                            double hash1_usage_threshold,
                            // NUMA-specific params
                            UInt32 num_numa_nodes,
                            NumaPolicy numa_policy,
                            double utilization_threshold,
                            UInt32 max_speculation_nodes,
                            const std::vector<UInt64>& per_node_capacity_mb,
                            const std::vector<UInt64>& per_node_kernel_mb)
        : PhysicalMemoryAllocator(name, memory_size, kernel_size)
        , m_max_order(max_order)
        , enable_aggressive_swapouts(_enable_aggressive_swapouts)
        , infrequency_threshold(_infrequency_threshold)
        , hash1_usage_threshold(hash1_usage_threshold)
        , m_num_numa_nodes(num_numa_nodes)
    {
        m_swap_mode_enabled = false;
        number_of_hashes = m_number_of_hashes;
        target_frag = _target_frag;
        m_memory_size = memory_size;
        m_kernel_size = kernel_size;
        number_of_pages_used = 0;

        // === Initialize NUMA placement engine ===
        m_placement_engine.init(num_numa_nodes, numa_policy,
                                 number_of_hashes,
                                 utilization_threshold,
                                 max_speculation_nodes);

        // === Set up per-node state ===
        m_node_states.resize(num_numa_nodes);
        UInt64 global_index = 0;

        for (UInt32 i = 0; i < num_numa_nodes; ++i)
        {
            UInt64 node_capacity_mb = (i < per_node_capacity_mb.size()) 
                                       ? per_node_capacity_mb[i] 
                                       : memory_size / num_numa_nodes;
            UInt64 node_kernel_mb = (i < per_node_kernel_mb.size())
                                     ? per_node_kernel_mb[i]
                                     : kernel_size / num_numa_nodes;

            UInt64 node_total_pages = node_capacity_mb * 1024 / 4;
            UInt64 node_kern_pages = node_kernel_mb * 1024 / 4;

            m_node_states[i].node_id = i;
            m_node_states[i].start_index = global_index;
            m_node_states[i].num_pages = node_total_pages - node_kern_pages;
            m_node_states[i].kernel_pages = node_kern_pages;

            // Configure placement engine node
            m_placement_engine.configureNode(
                i,
                (node_total_pages - node_kern_pages) * 4096,  // capacity_bytes (usable)
                node_kern_pages * 4096,                        // kernel_reserved_bytes
                global_index,                                  // start_pfn
                global_index + (node_total_pages - node_kern_pages), // end_pfn
                "ddr"                                          // default type
            );

            global_index += (node_total_pages - node_kern_pages);
        }

        m_total_pages = global_index;

        // Initialize flat allocation array
        memory_allocations.assign(m_total_pages,
            AllocationEntry{(IntPtr)-1, true, false, false, (UInt64)-1, 0});

        // Set node_id for each entry
        for (UInt32 i = 0; i < num_numa_nodes; ++i)
        {
            for (UInt64 j = 0; j < m_node_states[i].num_pages; ++j)
            {
                UInt64 idx = m_node_states[i].start_index + j;
                if (idx < memory_allocations.size())
                    memory_allocations[idx].node_id = i;
            }
        }

        // === Allocate stats arrays ===
        hash_stats.hits = (UInt64*)malloc(sizeof(UInt64) * number_of_hashes);
        hash_stats.tries = (UInt64*)malloc(sizeof(UInt64) * number_of_hashes);
        hash_stats.swap_attempts = (UInt64*)malloc(sizeof(UInt64) * number_of_hashes);
        hash_stats.swap_attempts_same_address = (UInt64*)malloc(sizeof(UInt64) * number_of_hashes);
        hash_stats.successful_pt_allocations = (UInt64*)malloc(sizeof(UInt64) * number_of_hashes);

        for (int i = 0; i < number_of_hashes; i++) {
            hash_stats.hits[i] = 0;
            hash_stats.tries[i] = 0;
            hash_stats.swap_attempts[i] = 0;
            hash_stats.swap_attempts_same_address[i] = 0;
            hash_stats.successful_pt_allocations[i] = 0;
        }

        // Per-NUMA-node stats
        m_numa_node_allocs = (UInt64*)calloc(num_numa_nodes, sizeof(UInt64));
        m_numa_node_spills = (UInt64*)calloc(num_numa_nodes, sizeof(UInt64));
        m_interleave_allocs = 0;
        m_local_allocs = 0;
        m_bind_allocs = 0;

        // Let the Policy handle stats registration
        Policy::register_stats(name, *this);

        // Policy-specific init
        Policy::on_init(name, memory_size, kernel_size, m_max_order, frag_type, this);
    }

    ~NumaRevelatorAllocator()
    {
        free(hash_stats.hits);
        free(hash_stats.tries);
        free(hash_stats.swap_attempts);
        free(hash_stats.swap_attempts_same_address);
        free(hash_stats.successful_pt_allocations);
        free(m_numa_node_allocs);
        free(m_numa_node_spills);
    }

    // =========================================================================
    // Accessors for Policy
    // =========================================================================

    int get_number_of_hashes() const { return number_of_hashes; }
    HashStats& get_hash_stats() { return hash_stats; }
    const HashStats& get_hash_stats() const { return hash_stats; }
    bool is_swap_mode_enabled() const { return m_swap_mode_enabled; }
    void enable_swap_mode() { m_swap_mode_enabled = true; }
    int get_infrequency_threshold() const { return infrequency_threshold; }
    bool aggressive_swapouts_enabled() const { return enable_aggressive_swapouts; }

    // NUMA accessors
    NumaPlacementEngine& getPlacementEngine() { return m_placement_engine; }
    const NumaPlacementEngine& getPlacementEngine() const { return m_placement_engine; }
    UInt32 getNumNumaNodes() const { return m_num_numa_nodes; }
    const NodeState& getNodeState(UInt32 node_id) const { return m_node_states[node_id]; }

    // Accessors for per-NUMA-node stats arrays (used by policy for registration)
    UInt64* getNumaNodeAllocStats() { return m_numa_node_allocs; }
    UInt64* getNumaNodeSpillStats() { return m_numa_node_spills; }
    UInt64& getLocalAllocCount() { return m_local_allocs; }
    UInt64& getBindAllocCount() { return m_bind_allocs; }
    UInt64& getInterleaveAllocCount() { return m_interleave_allocs; }

    void decrementNodePageUsed(UInt32 node_id) {
        if (node_id < m_num_numa_nodes && m_node_states[node_id].pages_used > 0) {
            m_node_states[node_id].pages_used--;
            m_placement_engine.recordDeallocation(node_id, 4096);
        }
    }

    struct VictimInfo {
        IntPtr address;
        UInt64 app_id;
    };

    VictimInfo get_victim_info(size_t idx) const {
        const auto& e = memory_allocations[idx];
        return VictimInfo{e.address, e.app_id};
    }

    // =========================================================================
    // NUMA-aware allocation
    // =========================================================================

    std::pair<UInt64, UInt64> allocate(UInt64 size,
                                       IntPtr address,
                                       UInt64 app_id,
                                       bool is_pagetable_allocation,
                                       bool is_instruction_allocation = false) override
    {
        // Instruction allocations: use reserved area
        if (is_instruction_allocation)
            return this->allocateInstruction(size);

        UInt64 vpn = is_pagetable_allocation ? (address >> 21) : (address >> 12);
        UInt64 to_hash = vpn;

        // === Policy hook: maybe enable swap mode ===
        Policy::maybe_enable_swap_mode(*this);

        // === Stage 1: Determine target NUMA node(s) ===
        UInt32 cpu_local_node = 0; // TODO: derive from core_id / app_id mapping
        UInt32 target_node = m_placement_engine.selectAllocationNode(vpn, cpu_local_node);

        // === Stage 2: Node-scoped hash-based allocation ===
        IntPtr hash = 0;
        auto& node = m_node_states[target_node];

        for (int i = 1; i <= number_of_hashes; i++)
        {
            hash_stats.tries[i - 1]++;

            // Node-scoped hash: hash within this node's page range
            hash = nodeHashFunction(i * to_hash, node.num_pages) + node.start_index;
            auto& entry = memory_allocations[hash];

            if (entry.is_free && !entry.is_poisoned)
            {
                entry.address = address;
                entry.is_free = false;
                entry.app_id = app_id;
                entry.is_pagetable_allocation = is_pagetable_allocation;
                entry.node_id = target_node;

                UInt64 page_address = hash + getGlobalKernelPages();
                number_of_pages_used++;
                node.pages_used++;
                node.alloc_count++;
                node.hash_hits++;
                hash_stats.hits[i - 1]++;
                m_numa_node_allocs[target_node]++;

                if (is_pagetable_allocation)
                    hash_stats.successful_pt_allocations[i - 1]++;

                // Set node hint for speculation
                m_placement_engine.setNodeHint(vpn, target_node);
                m_placement_engine.recordAllocation(target_node, 4096);

                trackPolicyAlloc(target_node);
                return std::make_pair(page_address, 12);
            }
            // === Policy-driven swapping ===
            else if (m_swap_mode_enabled &&
                     Policy::should_attempt_swap(*this, i, is_pagetable_allocation, entry))
            {
                bool swapped = Policy::try_swap_out_victim(
                    *this, hash, address, app_id, entry, i - 1);

                if (swapped)
                {
                    auto& slot = memory_allocations[hash];
                    slot.address = address;
                    slot.is_free = false;
                    slot.app_id = app_id;
                    slot.is_pagetable_allocation = is_pagetable_allocation;
                    slot.node_id = target_node;

                    UInt64 page_address = hash + getGlobalKernelPages();
                    number_of_pages_used++;
                    node.pages_used++;
                    m_placement_engine.setNodeHint(vpn, target_node);
                    m_placement_engine.recordAllocation(target_node, 4096);
                    trackPolicyAlloc(target_node);
                    return std::make_pair(page_address, 12);
                }
            }
        }

        // === Fallback: linear scan within target node ===
        {
            IntPtr index = hash;
            for (UInt64 i = 0; i < node.num_pages; i++)
            {
                index = node.start_index + ((index - node.start_index + 1) % node.num_pages);
                auto& entry = memory_allocations[index];

                if (entry.is_free && !entry.is_poisoned)
                {
                    entry.address = address;
                    entry.is_free = false;
                    entry.app_id = app_id;
                    entry.is_pagetable_allocation = is_pagetable_allocation;
                    entry.node_id = target_node;

                    UInt64 page_address = index + getGlobalKernelPages();
                    number_of_pages_used++;
                    node.pages_used++;
                    node.fallback_allocs++;
                    m_placement_engine.setNodeHint(vpn, target_node);
                    m_placement_engine.recordAllocation(target_node, 4096);
                    trackPolicyAlloc(target_node);
                    return std::make_pair(page_address, 12);
                }
            }
        }

        // === Spill to other NUMA nodes ===
        for (UInt32 n = 0; n < m_num_numa_nodes; ++n)
        {
            if (n == target_node) continue;
            auto& spill_node = m_node_states[n];

            for (UInt64 i = 0; i < spill_node.num_pages; i++)
            {
                UInt64 index = spill_node.start_index + i;
                auto& entry = memory_allocations[index];

                if (entry.is_free && !entry.is_poisoned)
                {
                    entry.address = address;
                    entry.is_free = false;
                    entry.app_id = app_id;
                    entry.is_pagetable_allocation = is_pagetable_allocation;
                    entry.node_id = n;

                    UInt64 page_address = index + getGlobalKernelPages();
                    number_of_pages_used++;
                    spill_node.pages_used++;
                    m_numa_node_spills[n]++;
                    m_placement_engine.setNodeHint(vpn, n);
                    m_placement_engine.recordAllocation(n, 4096);
                    return std::make_pair(page_address, 12);
                }
            }
        }

        log_file << "[NumaRevelator] Not enough space in any NUMA node." << std::endl;
        exit(1);
    }

    void deallocate(UInt64 index, UInt64 core_id) override
    {
        UInt64 local_index = index - getGlobalKernelPages();
        if (local_index >= memory_allocations.size()) return;

        auto& entry = memory_allocations[local_index];
        UInt32 node_id = entry.node_id;

        entry.is_free = true;
        entry.app_id = (UInt64)-1;
        entry.is_pagetable_allocation = false;
        entry.address = (IntPtr)-1;
        number_of_pages_used--;

        if (node_id < m_num_numa_nodes)
        {
            m_node_states[node_id].pages_used--;
            m_placement_engine.recordDeallocation(node_id, 4096);
        }

        // Clear node hint
        // (VPN not readily available here; could be derived from entry.address)
    }

    std::vector<Range> allocate_ranges(IntPtr, IntPtr, int) override { return {}; }

    void fragment_memory(double target_fragmentation) override
    {
        // Fragment each node independently
        perform_init_random(0, target_fragmentation, false, 0);
    }

    UInt64 givePageFast(UInt64 bytes, UInt64 address = 0, UInt64 core_id = (UInt64)-1) override
    {
        return allocate(bytes, address, core_id, false).first;
    }

    // =========================================================================
    // Node-scoped hashing
    // =========================================================================

    /**
     * Hash within a specific NUMA node's page range.
     * This ensures policy correctness: H_i only produces offsets within 
     * the target node's PFN range.
     */
    static UInt64 nodeHashFunction(IntPtr address, UInt64 node_page_count)
    {
        if (node_page_count == 0) return 0;
        return CityHash64((const char*)&address, 8) % node_page_count;
    }

    static UInt64 hashFunction(IntPtr address, int table_size)
    {
        return CityHash64((const char*)&address, 8) % table_size;
    }

    // =========================================================================
    // NUMA-aware prediction
    // =========================================================================
    // NOTE: Address prediction (predictNuma) is handled by the NumaRevelator
    // spec engine (spec_engine_designs/numa_revelator.h), which maintains its
    // own NumaHintTable (radix walker) and PTW-driven NumaNodeCounters.
    // The allocator only exposes per-node geometry so the spec engine can
    // compute node-scoped hashes.
    // =========================================================================

    /// Per-node geometry accessors (used by spec engine for hash-based prediction)
    const std::vector<NodeState>& getNodeStates() const { return m_node_states; }

    /**
     * @brief Authoritative PPN -> NUMA node lookup, in the allocator's own
     * geometry: [0, global_kernel_pages) is the kernel prefix (node 0), then
     * each node's usable pages follow contiguously.  MimicOS::getNumaNodeForPPN
     * routes the walker's per-node DRAM accounting through this.
     */
    UInt32 getNumaNodeForPPN(UInt64 ppn) const override
    {
        UInt64 kernel_pages = getGlobalKernelPages();
        if (ppn < kernel_pages) return 0;
        UInt64 usable_index = ppn - kernel_pages;
        for (const auto& ns : m_node_states)
            if (usable_index >= ns.start_index && usable_index < ns.start_index + ns.num_pages)
                return ns.node_id;
        return 0;
    }

    /// Get NUMA-specific stats
    UInt64 getNumaNodeAllocCount(UInt32 node_id) const {
        return (node_id < m_num_numa_nodes) ? m_numa_node_allocs[node_id] : 0;
    }
    UInt64 getNumaNodeSpillCount(UInt32 node_id) const {
        return (node_id < m_num_numa_nodes) ? m_numa_node_spills[node_id] : 0;
    }

private:
    friend Policy;

    int m_max_order;
    bool enable_aggressive_swapouts;
    int infrequency_threshold;
    double hash1_usage_threshold;
    double target_frag;

    UInt64 m_memory_size;
    UInt64 m_kernel_size;
    UInt64 m_total_pages;
    UInt64 number_of_pages_used = 0;

    int number_of_hashes;
    bool m_swap_mode_enabled;

    // NUMA state
    UInt32 m_num_numa_nodes;
    NumaPlacementEngine m_placement_engine;
    std::vector<NodeState> m_node_states;

    // Flat allocation table (all nodes concatenated)
    std::vector<AllocationEntry> memory_allocations;

    HashStats hash_stats;
    std::ofstream log_file;
    std::string log_file_name;
    std::ofstream* log_stream = nullptr;

    // NUMA stats
    UInt64* m_numa_node_allocs;
    UInt64* m_numa_node_spills;
    UInt64 m_interleave_allocs;
    UInt64 m_local_allocs;
    UInt64 m_bind_allocs;

    UInt64 getGlobalKernelPages() const
    {
        // Sum up kernel pages from all nodes
        UInt64 total = 0;
        for (const auto& ns : m_node_states)
            total += ns.kernel_pages;
        return total;
    }

    void trackPolicyAlloc(UInt32 target_node)
    {
        NumaPolicy p = m_placement_engine.getPolicy();
        if (p == NumaPolicy::LOCAL || p == NumaPolicy::PREFERRED)
            m_local_allocs++;
        else if (p == NumaPolicy::BIND)
            m_bind_allocs++;
        else if (p == NumaPolicy::INTERLEAVE)
            m_interleave_allocs++;
    }

    void perform_init_random(double target_fragmentation,
                              double target_memory_percent,
                              bool store_in_file,
                              UInt64 core_idx)
    {
        // Fragment each node independently
        double target_mem = 1.0f - target_memory_percent;

        for (UInt32 n = 0; n < m_num_numa_nodes; ++n)
        {
            auto& node = m_node_states[n];
            UInt64 pages_to_poison = (UInt64)(node.num_pages * target_mem);

            for (UInt64 i = 0; i < pages_to_poison; i++)
            {
                IntPtr hash_number = 1000;
                while (true)
                {
                    IntPtr hash = nodeHashFunction(i * hash_number, node.num_pages);
                    UInt64 idx = node.start_index + hash;
                    if (idx < memory_allocations.size() && !memory_allocations[idx].is_poisoned)
                    {
                        memory_allocations[idx] =
                            AllocationEntry{(IntPtr)-1, true, true, false, (UInt64)-1, n};
                        break;
                    }
                    hash_number++;
                }
                number_of_pages_used++;
                node.pages_used++;
            }
        }
    }
};
