#include "numa_revelator.h"
#include "simulator.h"
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "shmem_perf_model.h"
#include "pagetable.h"
#include "tlb_subsystem.h"
#include "cache_block_info.h"
#include "stats.h"
#include "mimicos.h"
#include "dvfs_manager.h"
#include "dram_cntlr_interface.h"
#include "memory_manager.h"

//#define DEBUG_NUMA_REVELATOR

namespace ParametricDramDirectoryMSI
{

// ========================================================================
// Constructor
// ========================================================================
NumaRevelator::NumaRevelator(Core *core, MemoryManagerBase *_memory_manager,
                              ShmemPerfModel *shmem_perf_model, String _name)
    : SpecEngineBase(core, _memory_manager, shmem_perf_model, _name)
    , name(_name)
    , memory_manager(_memory_manager)
    , m_hint_table(nullptr)
    , m_counters(nullptr)
{
#ifdef DEBUG_NUMA_REVELATOR
    std::cout << "[NumaRevelator] Initializing NUMA Revelator with hint table + counters" << std::endl;
#endif

    // ----------------------------------------------------------------
    // Base Revelator config
    // ----------------------------------------------------------------
    number_of_hashes      = Sim()->getCfg()->getInt("perf_model/revelator/number_of_hashes");
    number_of_predictions = Sim()->getCfg()->getInt("perf_model/revelator/number_of_predictions");
    m_memory_size         = (UInt64)Sim()->getCfg()->getInt("perf_model/revelator/memory_size");
    kernel_size           = Sim()->getCfg()->getInt("perf_model/revelator/kernel_size");
    oracle_revelator      = Sim()->getCfg()->getBool("perf_model/revelator/oracle");
    rev_type              = Sim()->getCfg()->getInt("perf_model/revelator/type");
    has_filter            = Sim()->getCfg()->getBool("perf_model/revelator/filter");
    perfect_filtering     = Sim()->getCfg()->getBool("perf_model/revelator/perfect_filtering");
    m_total_pages         = m_memory_size * 1024 / 4 - kernel_size * 1024 / 4;
    m_kernel_pages        = kernel_size * 1024 / 4;

    // ----------------------------------------------------------------
    // NUMA geometry (aligned with allocator model)
    // ----------------------------------------------------------------
    // Allocator model: flat array of usable-only pages per node,
    // global_kernel_pages is a prefix offset added to all PFNs.
    // PPN = global_kernel_pages + node.start_index + offset_in_node
    // ----------------------------------------------------------------
    m_num_numa_nodes = (UInt32)Sim()->getCfg()->getInt("perf_model/dram/numa/num_nodes");
    m_max_speculation_nodes = std::min(m_num_numa_nodes, (UInt32)2);

    m_per_node_usable_pages.resize(m_num_numa_nodes);
    m_per_node_start_index.resize(m_num_numa_nodes);
    m_per_node_end_index.resize(m_num_numa_nodes);

    // Compute per-node usable pages and global kernel prefix
    // Each node: usable = total - kernel, arranged contiguously
    UInt64 total_kernel_pages = 0;
    UInt64 cumulative_usable = 0;
    for (UInt32 n = 0; n < m_num_numa_nodes; ++n)
    {
        UInt64 node_total  = m_total_pages / m_num_numa_nodes;
        UInt64 node_kernel = m_kernel_pages / m_num_numa_nodes;

        m_per_node_usable_pages[n] = node_total;    // usable pages (total already excludes kernel in m_total_pages)
        m_per_node_start_index[n]  = cumulative_usable;
        cumulative_usable         += node_total;
        m_per_node_end_index[n]    = cumulative_usable;
        total_kernel_pages        += node_kernel;
    }
    m_global_kernel_pages = m_kernel_pages;  // kernel pages are a global prefix

    // ----------------------------------------------------------------
    // NUMA placement policy (for spec engine node selection)
    // ----------------------------------------------------------------
    m_numa_policy = "local";
    m_preferred_node = 0;
    try { m_numa_policy = Sim()->getCfg()->getString("perf_model/dram/numa/policy"); } catch (...) {}
    try { m_preferred_node = (UInt32)Sim()->getCfg()->getInt("perf_model/dram/numa/preferred_node"); } catch (...) {}

    // Build interleave mask (default: all nodes in order)
    m_interleave_mask.clear();
    for (UInt32 n = 0; n < m_num_numa_nodes; ++n)
        m_interleave_mask.push_back(n);

    // ----------------------------------------------------------------
    // Hint table (radix tree: VPN → NUMA node id, 2 bits)
    // ----------------------------------------------------------------
    int numa_bits = 2;  // supports up to 4 nodes
    m_hint_table = new NumaHintTable(numa_bits, /*page_size_bits=*/12);

    // ----------------------------------------------------------------
    // PTW-driven saturating counters
    // ----------------------------------------------------------------
    UInt32 counter_entries = 4096;   // number of hash buckets
    int delta_inc   = 2;             // increment on PTW match
    int delta_dec   = 1;             // decrement on PTW mismatch
    int confidence  = 4;             // gap threshold for confident prediction

    // Read from config if available (optional knobs)
    try { counter_entries = (UInt32)Sim()->getCfg()->getInt("perf_model/numa_revelator/counter_entries"); } catch (...) {}
    try { delta_inc       = Sim()->getCfg()->getInt("perf_model/numa_revelator/counter_delta_inc"); }      catch (...) {}
    try { delta_dec       = Sim()->getCfg()->getInt("perf_model/numa_revelator/counter_delta_dec"); }      catch (...) {}
    try { confidence      = Sim()->getCfg()->getInt("perf_model/numa_revelator/counter_confidence"); }     catch (...) {}
    try { m_max_speculation_nodes = (UInt32)Sim()->getCfg()->getInt("perf_model/numa_revelator/max_speculation_nodes"); } catch (...) {}

    m_counters = new NumaNodeCounters(counter_entries, m_num_numa_nodes,
                                      delta_inc, delta_dec, confidence);

    std::cout << "[NumaRevelator] nodes=" << m_num_numa_nodes
              << " max_spec_nodes=" << m_max_speculation_nodes
              << " counter_entries=" << counter_entries
              << " delta_inc=" << delta_inc
              << " delta_dec=" << delta_dec
              << " confidence=" << confidence
              << " hint_bits=" << numa_bits
              << " policy=" << m_numa_policy.c_str()
              << " preferred_node=" << m_preferred_node
              << " global_kernel_pages=" << m_global_kernel_pages
              << std::endl;

    // ----------------------------------------------------------------
    // Log file
    // ----------------------------------------------------------------
    log_file_name = std::string(name.c_str()) + ".numa_revelator.log";
    log_file_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str())
                  + "/" + log_file_name;
    log_file.open(log_file_name);

    // ----------------------------------------------------------------
    // Per-hash stats
    // ----------------------------------------------------------------
    hits_per_hash               = new UInt64[number_of_predictions]();
    prefetches_per_hash         = new UInt64[number_of_predictions]();
    hits_per_hash_pt            = new UInt64[number_of_predictions]();
    prefetches_per_hash_pt      = new UInt64[number_of_predictions]();
    filtered_predictions_per_hash = new UInt64[number_of_predictions]();

    for (int i = 0; i < number_of_predictions; i++)
    {
        registerStatsMetric("numa_revelator", core->getId(), "hits_" + itostr(i), &hits_per_hash[i]);
        registerStatsMetric("numa_revelator", core->getId(), "prefetches_" + itostr(i), &prefetches_per_hash[i]);
        registerStatsMetric("numa_revelator", core->getId(), "hits_pt_" + itostr(i), &hits_per_hash_pt[i]);
        registerStatsMetric("numa_revelator", core->getId(), "prefetches_pt_" + itostr(i), &prefetches_per_hash_pt[i]);
        registerStatsMetric("numa_revelator", core->getId(), "filtered_predictions_" + itostr(i), &filtered_predictions_per_hash[i]);
    }

    relevator_prefetches = 0;
    registerStatsMetric("numa_revelator", core->getId(), "total_prefetches", &relevator_prefetches);

    // ----------------------------------------------------------------
    // Per-node stats
    // ----------------------------------------------------------------
    per_node_predictions = new UInt64[m_num_numa_nodes]();
    per_node_hits        = new UInt64[m_num_numa_nodes]();
    for (UInt32 n = 0; n < m_num_numa_nodes; ++n)
    {
        registerStatsMetric("numa_revelator", core->getId(), "node" + itostr(n) + "_predictions", &per_node_predictions[n]);
        registerStatsMetric("numa_revelator", core->getId(), "node" + itostr(n) + "_hits", &per_node_hits[n]);
    }

    // ----------------------------------------------------------------
    // Hint / counter stats
    // ----------------------------------------------------------------
    registerStatsMetric("numa_revelator", core->getId(), "hint_lookups",         &m_hint_lookups);
    registerStatsMetric("numa_revelator", core->getId(), "hint_hits",            &m_hint_hits);
    registerStatsMetric("numa_revelator", core->getId(), "counter_confident",    &m_counter_confident);
    registerStatsMetric("numa_revelator", core->getId(), "counter_not_confident", &m_counter_not_confident);
    registerStatsMetric("numa_revelator", core->getId(), "train_updates",        &m_train_updates);
    registerStatsMetric("numa_revelator", core->getId(), "train_node_mismatch",  &m_train_node_mismatch);
}

NumaRevelator::~NumaRevelator()
{
    delete m_hint_table;
    delete m_counters;
    delete[] hits_per_hash;
    delete[] prefetches_per_hash;
    delete[] hits_per_hash_pt;
    delete[] prefetches_per_hash_pt;
    delete[] filtered_predictions_per_hash;
    delete[] per_node_predictions;
    delete[] per_node_hits;
}

// ========================================================================
// allocateInSpecEngine — TRAINING (called after PTW resolves VPN → PPN)
// ========================================================================
void NumaRevelator::allocateInSpecEngine(IntPtr address, IntPtr ppn,
                                          int count, Core::lock_signal_t lock,
                                          IntPtr eip, bool modeled)
{
    UInt64 vpn = address >> 12;
    UInt32 actual_node = ppnToNumaNode(ppn);

    if (actual_node >= m_num_numa_nodes)
        return;  // PPN not in any known NUMA range — skip training

    m_train_updates++;

    // (1) Check if the hint table already had a hint for this VPN
    int old_node = -1;
    bool had_hint = m_hint_table->lookup(vpn, old_node);
    if (had_hint && (UInt32)old_node != actual_node)
        m_train_node_mismatch++;

    // (2) Update hint table: VPN → actual NUMA node
    m_hint_table->set(vpn, (int)actual_node);

    // (3) Update PTW-driven saturating counters
    m_counters->update(vpn, actual_node);

#ifdef DEBUG_NUMA_REVELATOR
    log_file << "[Train] VPN=0x" << std::hex << vpn
             << " PPN=0x" << ppn
             << " node=" << std::dec << actual_node
             << (had_hint ? (old_node != (int)actual_node ? " MISMATCH" : " OK") : " NEW")
             << std::endl;
#endif
}

// ========================================================================
// invokeSpecEngine — PREDICTION (called at TLB miss / PTW time)
// ========================================================================
void NumaRevelator::invokeSpecEngine(IntPtr address, int count,
                                      Core::lock_signal_t lock, IntPtr eip,
                                      bool modeled, SubsecondTime invoke_start_time,
                                      IntPtr physical_address, bool page_table_speculation)
{
#ifdef DEBUG_NUMA_REVELATOR
    log_file << "[Predict] VA=0x" << std::hex << address
             << " PA=0x" << physical_address << std::dec << std::endl;
#endif

    if (page_table_speculation && rev_type == 1) return;
    if (!page_table_speculation && rev_type == 2) return;

    // Oracle mode: prefetch the known PA directly
    if (oracle_revelator)
    {
        IntPtr cache_address = physical_address & ~((IntPtr)63);
        memory_manager->getCacheCntlrAt(core->getId(), MemComponent::component_t::L2_CACHE)
            ->handleMMUPrefetch(eip, cache_address, invoke_start_time);
        return;
    }

    // Determine CPU-local NUMA node from core ID
    UInt32 cpu_local_node = 0;
    if (m_num_numa_nodes > 0)
    {
        UInt32 cores_total   = Sim()->getConfig()->getApplicationCores();
        UInt32 cores_per_node = (cores_total > 0 && m_num_numa_nodes > 0)
                                 ? cores_total / m_num_numa_nodes : 1;
        if (cores_per_node == 0) cores_per_node = 1;
        cpu_local_node = core->getId() / cores_per_node;
        if (cpu_local_node >= m_num_numa_nodes)
            cpu_local_node = m_num_numa_nodes - 1;
    }

    // Generate NUMA-scoped predictions using hint table + counters
    std::vector<NumaPrediction> predictions =
        predictNuma(address, number_of_predictions, page_table_speculation, cpu_local_node);

    relevator_prefetches++;

    for (size_t k = 0; k < predictions.size(); k++)
    {
        IntPtr cache_address = predictions[k].predicted_address & ~((IntPtr)63);
        int    hash_idx = predictions[k].hash_index;
        UInt32 node_id  = predictions[k].node_id;

        // Allocation-ratio filter (same as vanilla Revelator)
        if (has_filter && hash_idx > 0)
        {
            if (Sim()->getMimicOS()->getMemoryAllocator()->getAllocRatioForHash(hash_idx) < 0.1f)
            {
                if (hash_idx < number_of_predictions)
                    filtered_predictions_per_hash[hash_idx]++;
                continue;
            }
        }

        // Perfect filter: skip wrong predictions
        if (perfect_filtering && predictions[k].predicted_address != physical_address)
            continue;

        // Issue prefetch
        memory_manager->getCacheCntlrAt(core->getId(), MemComponent::component_t::L2_CACHE)
            ->handleMMUPrefetch(eip, cache_address, invoke_start_time);

        // Per-hash stats
        if (hash_idx < number_of_predictions)
        {
            if (page_table_speculation) prefetches_per_hash_pt[hash_idx]++;
            else                        prefetches_per_hash[hash_idx]++;
        }

        // Per-node stats
        if (node_id < m_num_numa_nodes)
            per_node_predictions[node_id]++;

        // Accuracy check (hit = predicted PA matches actual PA)
        if (predictions[k].predicted_address == physical_address)
        {
            if (hash_idx < number_of_predictions)
            {
                if (page_table_speculation) hits_per_hash_pt[hash_idx]++;
                else                        hits_per_hash[hash_idx]++;
            }
            if (node_id < m_num_numa_nodes)
                per_node_hits[node_id]++;
        }
    }
}

// ========================================================================
// selectCandidateNodes — hybrid confidence-driven selection + policy-aware
// ========================================================================
std::vector<UInt32>
NumaRevelator::selectCandidateNodes(UInt64 vpn, UInt32 cpu_local_node)
{
    // ---- Interleaved short-circuit: deterministic node, no hints/counters ----
    if (m_numa_policy == "interleaved")
    {
        std::vector<UInt32> v;
        v.reserve(1);
        v.push_back(getPrimaryNodeForPolicy(vpn, cpu_local_node));
        return v;
    }

    std::vector<UInt32> candidates;
    candidates.reserve(m_max_speculation_nodes);

    auto push_unique = [&](UInt32 n) {
        if (candidates.size() >= m_max_speculation_nodes) return;
        for (UInt32 c : candidates) if (c == n) return;
        candidates.push_back(n);
    };

    // ---- Step 0: counter confidence first (fast path) ----
    bool confident = m_counters->isConfident(vpn);
    if (confident) m_counter_confident++;
    else           m_counter_not_confident++;

    // ---- Step 1: policy-required "primary" node must always be included ----
    UInt32 primary = getPrimaryNodeForPolicy(vpn, cpu_local_node);
    push_unique(primary);

    // ---- Step 2: confident => counters only (skip hint table) ----
    if (confident)
    {
        std::vector<UInt32> top =
            m_counters->getTopKNodes(vpn, m_max_speculation_nodes);

        for (UInt32 cn : top)
            push_unique(cn);

        // Ensure CPU-local is present as last-resort for local/preferred
        if (shouldForceCpuLocalFallback())
            push_unique(cpu_local_node);

        return candidates;
    }

    // ---- Step 3: not confident => consult hint table ----
    m_hint_lookups++;
    int hinted_node = -1;
    bool hint_valid = m_hint_table->lookup(vpn, hinted_node);
    if (hint_valid && (UInt32)hinted_node < m_num_numa_nodes)
    {
        m_hint_hits++;
        push_unique((UInt32)hinted_node);
    }

    // ---- Step 4: add counter nodes as backup ----
    std::vector<UInt32> top =
        m_counters->getTopKNodes(vpn, m_max_speculation_nodes);

    for (UInt32 cn : top)
        push_unique(cn);

    // ---- Step 5: last resort fallback ----
    if (shouldForceCpuLocalFallback())
        push_unique(cpu_local_node);

    return candidates;
}

// ========================================================================
// predictNuma — generate per-node × per-hash Revelator predictions
//   PFN model (matches allocator):
//     global_pfn = m_global_kernel_pages + node.start_index + hash_within_usable
// ========================================================================
std::vector<NumaRevelator::NumaPrediction>
NumaRevelator::predictNuma(IntPtr address, int num_predictions,
                            bool is_page_table, UInt32 cpu_local_node)
{
    std::vector<NumaPrediction> predictions;

    UInt64 to_hash = is_page_table ? (address >> 21) : (address >> 12);
    UInt64 offset  = is_page_table ? ((address >> 12) & 0x1ff) * 8 : (address & 0xFFF);

    // Get candidate NUMA nodes from hybrid confidence-driven selection
    std::vector<UInt32> candidate_nodes = selectCandidateNodes(to_hash, cpu_local_node);

    // Generate predictions: for each hash_index × each candidate node
    for (int h = 0; h < std::min(num_predictions, number_of_hashes); ++h)
    {
        for (UInt32 node_id : candidate_nodes)
        {
            if (node_id >= m_num_numa_nodes) continue;

            UInt64 node_usable = m_per_node_usable_pages[node_id];
            if (node_usable == 0) continue;

            // Hash within node's usable page range
            UInt64 hash = nodeHashFunction(to_hash * (h + 1), node_usable);

            // Convert to global PFN: kernel prefix + node start + offset
            UInt64 global_pfn = m_global_kernel_pages
                              + m_per_node_start_index[node_id]
                              + hash;

            IntPtr predicted_pa = (global_pfn << 12) + offset;

#ifdef DEBUG_NUMA_REVELATOR
            log_file << "[Predict] h=" << h << " node=" << node_id
                     << " hash=" << hash << " global_pfn=" << global_pfn
                     << " pa=0x" << std::hex << predicted_pa << std::dec << std::endl;
#endif

            predictions.push_back(NumaPrediction{predicted_pa, node_id, h});
        }
    }

    return predictions;
}

// ========================================================================
// ppnToNumaNode — map PPN to NUMA node using allocator-aligned geometry
//   PPN layout: [0, global_kernel_pages) = kernel prefix
//               [global_kernel_pages + start_index[n], ... + end_index[n]) = node n usable
// ========================================================================
UInt32 NumaRevelator::ppnToNumaNode(IntPtr ppn) const
{
    if ((UInt64)ppn < m_global_kernel_pages)
        return m_num_numa_nodes;  // kernel prefix — not a NUMA usable page

    UInt64 usable_index = (UInt64)ppn - m_global_kernel_pages;
    for (UInt32 n = 0; n < m_num_numa_nodes; ++n)
    {
        if (usable_index >= m_per_node_start_index[n] && usable_index < m_per_node_end_index[n])
            return n;
    }
    return m_num_numa_nodes;  // not found
}

// ========================================================================
// getPrimaryNodeForPolicy — policy-aware primary node selection
// ========================================================================
UInt32 NumaRevelator::getPrimaryNodeForPolicy(UInt64 vpn, UInt32 cpu_local_node) const
{
    if (m_numa_policy == "interleaved")
    {
        if (m_interleave_mask.empty()) return cpu_local_node;
        UInt64 idx = vpn % m_interleave_mask.size();
        UInt32 node = m_interleave_mask[idx];
        return (node < m_num_numa_nodes) ? node : (cpu_local_node % m_num_numa_nodes);
    }
    else if (m_numa_policy == "preferred")
    {
        return (m_preferred_node < m_num_numa_nodes) ? m_preferred_node : 0;
    }
    // default: "local"
    return cpu_local_node;
}

// ========================================================================
// shouldForceCpuLocalFallback — interleaved uses deterministic node only
// ========================================================================
bool NumaRevelator::shouldForceCpuLocalFallback() const
{
    return (m_numa_policy != "interleaved");
}

// ========================================================================
// Hash functions
// ========================================================================
UInt64 NumaRevelator::hashFunction(IntPtr address, int table_size)
{
    return CityHash64((const char*)&address, 8) % table_size;
}

UInt64 NumaRevelator::nodeHashFunction(IntPtr address, UInt64 node_table_size)
{
    if (node_table_size == 0) return 0;
    return CityHash64((const char*)&address, 8) % node_table_size;
}

// ========================================================================
// Legacy fallback
// ========================================================================
std::vector<IntPtr>
NumaRevelator::predict(IntPtr address, int number_of_predictions, bool is_page_table)
{
    auto numa_preds = predictNuma(address, number_of_predictions, is_page_table, 0);
    std::vector<IntPtr> result;
    result.reserve(numa_preds.size());
    for (auto& p : numa_preds)
        result.push_back(p.predicted_address);
    return result;
}

} // namespace ParametricDramDirectoryMSI
