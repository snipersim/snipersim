// ============================================================================
// Victima MMU — leaf-probe-first PTW path over real cache hierarchy
// ============================================================================

#include "mmu_victima.h"
#include "memory_manager.h"
#include "shmem_perf_model.h"
#include "config.hpp"
#include "stats.h"
#include "simulator.h"
#include "mimicos.h"
#include "thread.h"
#include "spec_engine_factory.h"

#include <algorithm>

namespace ParametricDramDirectoryMSI
{

MemoryManagementUnitVictima::MemoryManagementUnitVictima(
    Core* core_, MemoryManagerBase* memory_manager_, ShmemPerfModel* shmem_perf_model_,
    String name_, MemoryManagementUnitBase* nested_mmu_)
    : MemoryManagementUnit(core_, memory_manager_, shmem_perf_model_, name_, nested_mmu_)
    , m_victima_enabled(true)
    , m_spec_engine(nullptr)
    , m_spec_invocations(0)
    , m_spec_correct(0)
    , m_victima_leaf_probes(0)
    , m_victima_leaf_hits(0)
    , m_victima_leaf_hits_l1(0)
    , m_victima_leaf_hits_l2(0)
    , m_victima_leaf_hits_llc(0)
    , m_victima_leaf_misses(0)
    , m_victima_bg_fetches(0)
    , m_victima_leaf_probe_latency(SubsecondTime::Zero())
    , m_victima_upper_walk_latency(SubsecondTime::Zero())
    , m_parallel_probe_enabled(false)
    , m_pp_eff_l2_cyc(0)
    , m_pp_eff_nuca_cyc(8)
    , m_pp_eff_miss_cyc(3)
    , m_victima_pp_l2_hits(0)
    , m_victima_pp_nuca_hits(0)
    , m_victima_pp_misses(0)
{
    m_victima_enabled = Sim()->getCfg()->getBoolDefault("perf_model/" + name + "/victima/enabled", true);
    m_bg_fetch_enabled = Sim()->getCfg()->getBoolDefault("perf_model/" + name + "/victima/bg_fetch_enabled", true);
    m_leaf_probe_enabled = Sim()->getCfg()->getBoolDefault("perf_model/" + name + "/victima/leaf_probe_enabled", true);

    // Parallel-probe (tag-only) knobs.  Effective cycles charged on the
    // critical path per hit_where, after L2-TLB-lookup overlap.  Defaults
    // assume meteor_lake_pcore + tlb_hierarchy_shared (L2 TLB=12, L2 data=12,
    // NUCA data=20, NUCA tag=15 cyc):
    //   L2 hit  -> 0 cyc  (12 - 12)
    //   NUCA hit-> 8 cyc  (20 - 12)
    //   miss    -> 3 cyc  (15 - 12; tag-only check, walker does the leaf fetch)
    m_parallel_probe_enabled = Sim()->getCfg()->getBoolDefault("perf_model/" + name + "/victima/parallel_probe", false);
    auto cfgInt = [&](const String& key, int defv) -> int {
        return Sim()->getCfg()->hasKey(key) ? (int)Sim()->getCfg()->getInt(key) : defv;
    };
    m_pp_eff_l2_cyc   = cfgInt("perf_model/" + name + "/victima/pp_eff_l2_cyc",   0);
    m_pp_eff_nuca_cyc = cfgInt("perf_model/" + name + "/victima/pp_eff_nuca_cyc", 8);
    m_pp_eff_miss_cyc = cfgInt("perf_model/" + name + "/victima/pp_eff_miss_cyc", 3);

    if (Sim()->getCfg()->hasKey("perf_model/mmu/spec/type"))
    {
        String spec_type = Sim()->getCfg()->getString("perf_model/mmu/spec/type");
        if (spec_type != "" && spec_type != "none")
        {
            m_spec_engine = SpecEngineFactory::createSpecEngineBase(
                spec_type, core, MemoryManagementUnitBase::memory_manager, shmem_perf_model, "spec_engine");
        }
    }

    registerStatsMetric(name, core->getId(), "fullstack_spec_invocations", &m_spec_invocations);
    registerStatsMetric(name, core->getId(), "fullstack_spec_correct", &m_spec_correct);
    registerStatsMetric(name, core->getId(), "victima_leaf_probes", &m_victima_leaf_probes);
    registerStatsMetric(name, core->getId(), "victima_leaf_hits", &m_victima_leaf_hits);
    registerStatsMetric(name, core->getId(), "victima_leaf_hits_l1", &m_victima_leaf_hits_l1);
    registerStatsMetric(name, core->getId(), "victima_leaf_hits_l2", &m_victima_leaf_hits_l2);
    registerStatsMetric(name, core->getId(), "victima_leaf_hits_llc", &m_victima_leaf_hits_llc);
    registerStatsMetric(name, core->getId(), "victima_leaf_misses", &m_victima_leaf_misses);
    registerStatsMetric(name, core->getId(), "victima_bg_fetches", &m_victima_bg_fetches);
    registerStatsMetric(name, core->getId(), "victima_leaf_probe_latency", &m_victima_leaf_probe_latency);
    registerStatsMetric(name, core->getId(), "victima_upper_walk_latency", &m_victima_upper_walk_latency);
    registerStatsMetric(name, core->getId(), "victima_pp_l2_hits",   &m_victima_pp_l2_hits);
    registerStatsMetric(name, core->getId(), "victima_pp_nuca_hits", &m_victima_pp_nuca_hits);
    registerStatsMetric(name, core->getId(), "victima_pp_misses",    &m_victima_pp_misses);
}

// Tag-only parallel probe: peek L2 then NUCA.  No fill, no DRAM, no cache
// pollution from speculation.  Effective per-hit-where charges already bake
// in the L2-TLB-lookup overlap.
SubsecondTime MemoryManagementUnitVictima::victimaParallelProbe(
    IntPtr leaf_addr, HitWhere::where_t& out_hw, bool count)
{
    IntPtr line_addr = leaf_addr & ~((IntPtr)63);
    UInt32 cyc;

    auto* mm = MemoryManagementUnitBase::memory_manager;
    auto* l2 = mm->getCacheCntlrAt(core->getId(), MemComponent::component_t::L2_CACHE);
    SubsecondTime t_now = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);
    if (l2 && l2->isLinePresent(line_addr))
    {
        out_hw = HitWhere::L2_OWN;
        cyc = m_pp_eff_l2_cyc;
        if (count) m_victima_pp_l2_hits++;
    }
    else
    {
        NucaCache* nuca = mm->getNucaCache();
        if (nuca && nuca->getCache()->accessSingleLine(
                        line_addr, Cache::LOAD, /*buff=*/nullptr, /*bytes=*/0, t_now,
                        /*update_replacement=*/true))
        {
            out_hw = HitWhere::NUCA_CACHE;
            cyc = m_pp_eff_nuca_cyc;
            if (count) m_victima_pp_nuca_hits++;
        }
        else
        {
            out_hw = HitWhere::DRAM;  // probe miss -- walker will fetch leaf
            cyc = m_pp_eff_miss_cyc;
            if (count) m_victima_pp_misses++;
        }
    }
    return ComponentLatency(core->getDvfsDomain(), cyc).getLatency();
}

void MemoryManagementUnitVictima::onTLBLevelEviction(
    int level, IntPtr evicted_address, int evicted_page_size, IntPtr /*evicted_ppn*/,
    IntPtr eip, Core::lock_signal_t lock, bool instruction)
{
    // Only fire for L2 TLB evictions (level 1) and only when Victima is enabled.
    if (!m_victima_enabled || !m_bg_fetch_enabled || level != 1) return;

    int app_id = core->getThread()->getAppId();
    PageTable* pt = Sim()->getMimicOS()->getPageTable(app_id);
    if (!pt) return;

    // Functional walk to find the evicted VPN's leaf PTE address.
    // count=false (don't pollute walk stats); is_prefetch=true (don't trigger
    // demand-path side effects).
    PTWResult ptw_result;
    try {
        ptw_result = pt->initializeWalk(evicted_address, /*count=*/false,
                                        /*is_prefetch=*/true, /*restart_walk=*/false);
    } catch (...) {
        return;
    }
    if (ptw_result.fault_happened) return;

    IntPtr leaf_addr = 0;
    for (const auto& acc : ptw_result.accesses)
    {
        if (acc.is_pte) { leaf_addr = acc.physical_addr; break; }
    }
    if (leaf_addr == 0) return;

    // accessCache restores elapsed time after the call, so the demand thread
    // is not advanced.  packet.modeled=false / packet.count=false keeps cache
    // counters from being polluted.  The line is fetched into L1D→L2→LLC and
    // tagged VICTIMA_TLB_BLOCK by the tagCachesBlockType() inside accessCache.
    translationPacket packet;
    packet.eip = eip;
    packet.instruction = instruction;
    packet.lock_signal = lock;
    packet.modeled = false;
    packet.count = false;
    packet.address = leaf_addr;
    packet.type = CacheBlockInfo::block_type_t::VICTIMA_TLB_BLOCK;

    HitWhere::where_t hw = HitWhere::UNKNOWN;
    SubsecondTime now = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);
    (void)accessCache(packet, now, /*is_prefetch=*/true, hw);

    m_victima_bg_fetches++;
}

PTWOutcome MemoryManagementUnitVictima::performPTW(
    IntPtr address, bool modeled, bool count, bool is_prefetch,
    IntPtr eip, Core::lock_signal_t lock, PageTable* page_table,
    bool restart_walk, bool instruction)
{
    SubsecondTime walk_start = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

    // Functional walk first (handles page faults, computes leaf address).
    PTWResult ptw_result = page_table->initializeWalk(address, count, is_prefetch, restart_walk);

    // Mirror base performPTW: dedupe accesses.
    accessedAddresses uniq = ptw_result.accesses;
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    ptw_result.accesses = uniq;

    int page_size = ptw_result.page_size;
    IntPtr ppn_result = ptw_result.ppn;
    bool is_pagefault = ptw_result.fault_happened;
    int requested_frames = ptw_result.requested_frames;

    auto* mimicos = Sim()->getMimicOS();
    core_id_t pf_core_id = getCore()->getId();
    mimicos->setIsPageFault(pf_core_id, is_pagefault);
    mimicos->setNumRequestedFrames(pf_core_id, requested_frames);

    if (is_pagefault)
    {
        mimicos->setVaTriggeredPageFault(pf_core_id, address);
        // Page-fault path: charge nothing here; caller handles fault latency.
        return PTWOutcome(SubsecondTime::Zero(), true, ppn_result, page_size, requested_frames, ptw_result.payload_bits);
    }

    // ---------------- Leaf probe ----------------
    IntPtr leaf_addr = 0;
    for (const auto& acc : ptw_result.accesses)
    {
        if (acc.is_pte) { leaf_addr = acc.physical_addr; break; }
    }

    SubsecondTime walk_latency = SubsecondTime::Zero();

    if (leaf_addr == 0 || !m_victima_enabled || !m_leaf_probe_enabled)
    {
        // No leaf in result, Victima disabled, or leaf-probe disabled —
        // fall back to vanilla full walk (true baseline behaviour).
        if (nested_mmu == nullptr)
            ptw_result = filterPTWResult(address, ptw_result, page_table, count);
        walk_latency = calculatePTWCycles(ptw_result, count, modeled, eip, lock, address, instruction, is_prefetch);

        if (m_spec_engine)
        {
            IntPtr pa = (ppn_result << page_size) | (address & ((IntPtr(1) << page_size) - 1));
            m_spec_engine->invokeSpecEngine(address, count, lock, eip, modeled, walk_start, pa);
            m_spec_engine->allocateInSpecEngine(address, ppn_result, count, lock, eip, modeled);
            if (count)
            {
                m_spec_invocations++;
                if (m_spec_engine->wasLastPredictionCorrect()) m_spec_correct++;
            }
        }

        return PTWOutcome(walk_latency, false, ppn_result, page_size, requested_frames, ptw_result.payload_bits);
    }

    HitWhere::where_t hw = HitWhere::UNKNOWN;
    SubsecondTime leaf_latency;

    if (m_parallel_probe_enabled)
    {
        // ---- Tag-only parallel probe (peek L2 + NUCA, no fill, no DRAM) ----
        leaf_latency = victimaParallelProbe(leaf_addr, hw, count);
        if (count)
        {
            m_victima_leaf_probes++;
            m_victima_leaf_probe_latency += leaf_latency;
        }
    }
    else
    {
        // ---- Sequential probe via accessCache (legacy: fills on miss) ----
        translationPacket packet;
        packet.eip = eip;
        packet.instruction = instruction;
        packet.lock_signal = lock;
        packet.modeled = modeled;
        packet.count = count;
        packet.address = leaf_addr;
        packet.type = CacheBlockInfo::block_type_t::VICTIMA_TLB_BLOCK;

        SubsecondTime t_now = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);
        leaf_latency = accessCache(packet, t_now, /*is_prefetch=*/false, hw);
        if (count)
        {
            m_victima_leaf_probes++;
            m_victima_leaf_probe_latency += leaf_latency;
        }
    }

    bool leaf_in_cache = (hw == HitWhere::L1_OWN ||
                          hw == HitWhere::L2_OWN ||
                          hw == HitWhere::NUCA_CACHE ||
                          hw == HitWhere::L3_OWN);

    if (leaf_in_cache)
    {
        // Probe hit -- skip the radix walk entirely.
        walk_latency = leaf_latency;
        if (count)
        {
            m_victima_leaf_hits++;
            if (hw == HitWhere::L1_OWN) m_victima_leaf_hits_l1++;
            else if (hw == HitWhere::L2_OWN) m_victima_leaf_hits_l2++;
            else m_victima_leaf_hits_llc++;
        }
    }
    else
    {
        // Probe miss.
        //   * parallel-probe (tag-only) mode: walker did not fetch anything --
        //     run the FULL walk including leaf, then add the small probe
        //     overhead.
        //   * legacy mode: probe via accessCache filled the leaf, so walker
        //     handles only upper levels.
        PTWResult upper = ptw_result;
        if (!m_parallel_probe_enabled)
        {
            accessedAddresses upper_only;
            upper_only.reserve(ptw_result.accesses.size());
            for (const auto& acc : ptw_result.accesses)
                if (!acc.is_pte) upper_only.push_back(acc);
            upper.accesses = std::move(upper_only);
        }

        SubsecondTime upper_latency = SubsecondTime::Zero();
        if (!upper.accesses.empty())
        {
            if (nested_mmu == nullptr)
                upper = filterPTWResult(address, upper, page_table, count);
            upper_latency = calculatePTWCycles(upper, count, modeled, eip, lock, address, instruction, is_prefetch);
        }

        walk_latency = upper_latency + leaf_latency;
        if (count)
        {
            m_victima_leaf_misses++;
            m_victima_upper_walk_latency += upper_latency;
        }
    }

    // Optional Revelator-style spec engine: invoke after walk so the engine's
    // prefetch overlaps with the walk window (anchored at walk_start).
    if (m_spec_engine && !is_pagefault)
    {
        IntPtr pa = (ppn_result << page_size) | (address & ((IntPtr(1) << page_size) - 1));
        m_spec_engine->invokeSpecEngine(address, count, lock, eip, modeled, walk_start, pa);
        m_spec_engine->allocateInSpecEngine(address, ppn_result, count, lock, eip, modeled);
        if (count)
        {
            m_spec_invocations++;
            if (m_spec_engine->wasLastPredictionCorrect()) m_spec_correct++;
        }
    }

    return PTWOutcome(walk_latency, false, ppn_result, page_size, requested_frames, ptw_result.payload_bits);
}

}  // namespace ParametricDramDirectoryMSI
