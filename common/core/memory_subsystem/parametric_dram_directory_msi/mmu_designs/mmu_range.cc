// ============================================================================
// Range MMU: Redundant Memory Mappings [Karakostas et al., ISCA 2015]
// ============================================================================

#include "mmu_range.h"
#include "memory_manager.h"
#include "shmem_perf_model.h"
#include "config.hpp"
#include "stats.h"
#include "simulator.h"
#include "mimicos.h"
#include "thread.h"
#include "rangetable.h"
#include "debug_config.h"

#include <cstring>

namespace ParametricDramDirectoryMSI
{

RangeMMU::RangeMMU(Core *core_, MemoryManagerBase *memory_manager_, ShmemPerfModel *shmem_perf_model_,
                   String name_, MemoryManagementUnitBase *nested_mmu_)
    : MemoryManagementUnit(core_, memory_manager_, shmem_perf_model_, name_, nested_mmu_)
    , m_rlb(NULL)
{
    m_log = new SimLog("MMU_RANGE", core->getId(), DEBUG_MMU_RANGE);
    m_stats = {};

    m_rlb_enabled = Sim()->getCfg()->getBool("perf_model/" + name + "/rlb/enabled");
    int entries = Sim()->getCfg()->getInt("perf_model/" + name + "/rlb/num_sets");
    ComponentLatency latency(core->getDvfsDomain(), Sim()->getCfg()->getInt("perf_model/" + name + "/rlb/latency"));
    m_rlb = new RLB(core, "rlb", latency, entries);
    m_log->log("Range MMU: RLB of " + std::to_string(entries) + " entries" + (m_rlb_enabled ? "" : " (disabled)"));

    registerStatsMetric(name, core->getId(), "requests_resolved_by_rlb", &m_stats.requests_resolved_by_rlb);
    registerStatsMetric(name, core->getId(), "requests_resolved_by_range_table", &m_stats.requests_resolved_by_range_table);
    registerStatsMetric(name, core->getId(), "range_table_misses", &m_stats.range_table_misses);
    registerStatsMetric(name, core->getId(), "requests_resolved_by_rlb_latency", &m_stats.requests_resolved_by_rlb_latency);
    registerStatsMetric(name, core->getId(), "total_range_walk_latency", &m_stats.total_range_walk_latency);
}

RangeMMU::~RangeMMU()
{
    delete m_rlb;
    delete m_log;
}

bool RangeMMU::lookupBeyondTLBs(
    IntPtr address, IntPtr eip, bool instruction,
    Core::lock_signal_t lock, bool modeled, bool count,
    SubsecondTime &latency, IntPtr &ppn, int &page_size)
{
    if (!m_rlb_enabled)
        return false;

    // The RLB first: its lookup latency is paid whether it hits or not.
    latency = m_rlb->getLatency();
    auto rlb = m_rlb->lookup(address, count);
    RLB::Entry range = rlb.second;

    if (!rlb.first)
    {
        // The range table in memory: every node visited is a line read
        // through the cache hierarchy, one after the other.
        RangeTable *range_table = Sim()->getMimicOS()->getRangeTable(core->getThread()->getAppId());
        if (range_table == NULL)
            return false;
        auto result = range_table->lookup(address);
        TreeNode *node = std::get<0>(result);
        if (node == NULL)
        {
            if (count)
                m_stats.range_table_misses++;
            m_log->debug("No range for", SimLog::hex(address), ": page table walk");
            return false;
        }
        int slot = std::get<1>(result);
        range.base = node->keys[slot].first;
        range.end = node->keys[slot].second;
        range.ppn = node->values[slot].offset;

        SubsecondTime t_now = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD) + latency;
        SubsecondTime walk_latency = SubsecondTime::Zero();
        for (IntPtr node_address : std::get<2>(result))
        {
            translationPacket packet;
            packet.address = node_address;
            packet.eip = eip;
            packet.instruction = instruction;
            packet.lock_signal = lock;
            packet.modeled = modeled;
            packet.count = count;
            packet.type = CacheBlockInfo::block_type_t::PAGE_TABLE_DATA;
            HitWhere::where_t where = HitWhere::UNKNOWN;
            walk_latency += accessCache(packet, t_now + walk_latency, false, where);
        }
        latency += walk_latency;
        if (count)
        {
            m_stats.requests_resolved_by_range_table++;
            m_stats.total_range_walk_latency += walk_latency;
        }
        m_rlb->insert(range);
        m_log->debug("Range table resolved", SimLog::hex(address), "in", SimLog::hex(range.base), "-",
                     SimLog::hex(range.end), "after", std::get<2>(result).size(), "node reads");
    }
    else if (count)
    {
        m_stats.requests_resolved_by_rlb++;
        m_stats.requests_resolved_by_rlb_latency += latency;
    }

    // PPN = frame of the range's base + the page's distance from it.
    ppn = range.ppn + ((address - range.base) >> 12);
    page_size = 12;
    return true;
}

}
