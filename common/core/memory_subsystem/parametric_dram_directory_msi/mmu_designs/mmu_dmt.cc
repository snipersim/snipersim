// ============================================================================
// Direct Memory Translation (DMT) MMU: a walk is one access of the leaf PTE
// ============================================================================

#include "mmu_dmt.h"
#include "memory_manager.h"
#include "shmem_perf_model.h"
#include "config.hpp"
#include "stats.h"
#include "simulator.h"
#include "mimicos.h"
#include "debug_config.h"

#include <algorithm>

namespace ParametricDramDirectoryMSI
{

MemoryManagementUnitDMT::MemoryManagementUnitDMT(
    Core *core_, MemoryManagerBase *memory_manager_, ShmemPerfModel *shmem_perf_model_,
    String name_, MemoryManagementUnitBase *nested_mmu_)
    : MemoryManagementUnit(core_, memory_manager_, shmem_perf_model_, name_, nested_mmu_)
    , m_leaf_accesses(0)
    , m_fallback_walks(0)
{
    m_log = new SimLog("MMU_DMT", core->getId(), DEBUG_MMU_DMT);
    registerStatsMetric(name, core->getId(), "dmt_leaf_accesses", &m_leaf_accesses);
    registerStatsMetric(name, core->getId(), "dmt_fallback_walks", &m_fallback_walks);
    m_log->log("Direct Memory Translation MMU for core " + std::to_string(core->getId()));
}

MemoryManagementUnitDMT::~MemoryManagementUnitDMT()
{
    delete m_log;
}

PTWOutcome MemoryManagementUnitDMT::performPTW(
    IntPtr address, bool modeled, bool count, bool is_prefetch,
    IntPtr eip, Core::lock_signal_t lock, PageTable *page_table,
    bool restart_walk, bool instruction)
{
    m_log->section("Starting direct translation");
    m_log->debug("Translation for address:", SimLog::hex(address));

    // The functional walk: page size, PPN, fault, and every entry a radix
    // walk would have visited.  Deduplicated as the baseline's is.
    PTWResult ptw_result = page_table->initializeWalk(address, count, is_prefetch, restart_walk);
    accessedAddresses visited = ptw_result.accesses;
    std::sort(visited.begin(), visited.end());
    visited.erase(std::unique(visited.begin(), visited.end()), visited.end());

    int page_size = ptw_result.page_size;
    IntPtr ppn_result = ptw_result.ppn;
    bool is_pagefault = ptw_result.fault_happened;
    int requested_frames = ptw_result.requested_frames;

    // The payload plane the translation came from, as the baseline reads it.
    PayloadWord leaf_payload_bits;
    if (!is_pagefault) {
        if (page_size == 21)
            leaf_payload_bits = page_table->readPMDPayloadBits(address >> 21);
        else
            leaf_payload_bits = page_table->readPayloadBits(address >> 12);
    }

    auto *mimicos = Sim()->getMimicOS();
    core_id_t pf_core_id = getCore()->getId();
    mimicos->setIsPageFault(pf_core_id, is_pagefault);
    mimicos->setNumRequestedFrames(pf_core_id, requested_frames);
    if (is_pagefault) {
        mimicos->setVaTriggeredPageFault(pf_core_id, address);
        m_log->debug("Page fault for address:", SimLog::hex(address), "requested frames:", requested_frames);
    }

    // The one entry DMT reads: the leaf (is_pte), or when the walk marked
    // none -- a faulting walk stops at an entry that is not a valid PTE --
    // the deepest entry it reached, which is the one a direct lookup would
    // have read.  The sort above orders by table then depth, so the deepest
    // is the last.
    accessedAddresses leaf;
    for (const auto &acc : visited)
        if (acc.is_pte) { leaf.push_back(acc); break; }
    if (leaf.empty() && !visited.empty())
        leaf.push_back(visited.back());

    PTWResult direct = ptw_result;
    direct.accesses = leaf;

    // Charged under the baseline's rules: a fault-free walk always, a
    // faulting demand walk only when fault latency is counted, a faulting
    // prefetch walk (which resolved its page through restart_walk) always.
    bool charge = !is_pagefault || count_page_fault_latency_enabled || is_prefetch;
    SubsecondTime ptw_cycles = SubsecondTime::Zero();
    if (charge)
    {
        if (leaf.empty())
        {
            // Nothing to read directly: the baseline walk, filtered as the
            // baseline filters it.
            if (count)
                m_fallback_walks++;
            ptw_result.accesses = visited;
            stats.memory_accesses_before_filtering += visited.size();
            if (nested_mmu == nullptr)
                ptw_result = filterPTWResult(address, ptw_result, page_table, count);
            ptw_cycles = calculatePTWCycles(ptw_result, count, modeled, eip, lock, address, instruction, is_prefetch);
        }
        else
        {
            if (count)
                m_leaf_accesses++;
            stats.memory_accesses_before_filtering += 1;
            m_log->debug("Leaf entry at:", SimLog::hex(leaf[0].physical_addr),
                         "table:", leaf[0].table_level, "depth:", leaf[0].depth);
            // No page walk cache: there is one access and nothing above it
            // to cache.  calculatePTWCycles charges the one line and keeps
            // the walker's accounting (where it hit, beyond-L2 fetches).
            ptw_cycles = calculatePTWCycles(direct, count, modeled, eip, lock, address, instruction, is_prefetch);
        }
    }

    m_log->debug("Direct translation latency:", ptw_cycles, "PPN:", SimLog::hex(ppn_result), "page size:", page_size);
    m_log->debug("-------------- End of direct translation");

    return PTWOutcome(ptw_cycles, is_pagefault, ppn_result, page_size, requested_frames, leaf_payload_bits);
}

}
