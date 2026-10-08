// ============================================================================
// Part-of-Memory TLB (POM-TLB) MMU: a very large TLB in main memory
// ============================================================================

#include "mmu_pomtlb.h"
#include "memory_manager.h"
#include "shmem_perf_model.h"
#include "config.hpp"
#include "stats.h"
#include "simulator.h"
#include "mimicos.h"
#include "memory_management/physical_memory_allocators/physical_memory_allocator.h"
#include "debug_config.h"

#include <algorithm>
#include <cstring>

namespace ParametricDramDirectoryMSI
{

MemoryManagementUnitPOMTLB::MemoryManagementUnitPOMTLB(
    Core *core_, MemoryManagerBase *memory_manager_, ShmemPerfModel *shmem_perf_model_,
    String name_, MemoryManagementUnitBase *nested_mmu_)
    : MemoryManagementUnit(core_, memory_manager_, shmem_perf_model_, name_, nested_mmu_)
{
    m_log = new SimLog("MMU_POMTLB", core->getId(), DEBUG_MMU_POMTLB);
    m_stats = {};

    // One POM-TLB per page size, from [perf_model/pom_tlb]: its tags and
    // translations are a TLB object (statistics under pom_tlb_<i>), its
    // entries a region of physical memory the OS's allocator hands out,
    // entries * entry bytes of it.  The TLB computes the entry size from
    // its geometry (tag plus a 52-bit translation, in bytes).
    int page_sizes = Sim()->getCfg()->getInt("perf_model/pom_tlb/page_sizes");
    for (int i = 0; i < page_sizes; i++)
    {
        PomTLB pom;
        pom.page_size = Sim()->getCfg()->getIntArray("perf_model/pom_tlb/page_size_list", i);
        int entries = Sim()->getCfg()->getIntArray("perf_model/pom_tlb/entries", i);
        int assoc = Sim()->getCfg()->getIntArray("perf_model/pom_tlb/assoc", i);
        String type = Sim()->getCfg()->getStringArray("perf_model/pom_tlb/type", i);
        bool allocate_on_miss = Sim()->getCfg()->getBoolArray("perf_model/pom_tlb/allocate_on_miss", i);

        int *page_size_list = new int[1];
        page_size_list[0] = pom.page_size;
        // No access latency of its own: a lookup costs the memory access
        // of its set.
        pom.tlb = new TLB("pom_tlb_" + itostr(i), "perf_model/pom_tlb", core->getId(),
                          ComponentLatency(core->getDvfsDomain(), 0),
                          entries, assoc, page_size_list, 1, type, allocate_on_miss);

        UInt64 bytes = (UInt64)entries * pom.tlb->getEntrySize();
        IntPtr base_frame = Sim()->getMimicOS()->getMemoryAllocator()->handle_page_table_allocations(bytes);
        pom.base_address = base_frame * 4096;

        m_log->log("POM-TLB " + std::to_string(i) + ": page size " + std::to_string(pom.page_size) +
                   ", " + std::to_string(entries) + " entries, " + std::to_string(assoc) + " ways, " +
                   std::to_string(pom.tlb->getEntrySize()) + " bytes per entry, " +
                   std::to_string(bytes) + " bytes at " + SimLog::hex(pom.base_address));
        m_pom_tlbs.push_back(pom);
    }

    registerStatsMetric(name, core->getId(), "software_tlb_lookups", &m_stats.lookups);
    registerStatsMetric(name, core->getId(), "software_tlb_hits", &m_stats.hits);
    registerStatsMetric(name, core->getId(), "software_tlb_misses", &m_stats.misses);
    registerStatsMetric(name, core->getId(), "software_tlb_installs", &m_stats.installs);
    registerStatsMetric(name, core->getId(), "total_software_tlb_latency", &m_stats.software_tlb_latency);
}

MemoryManagementUnitPOMTLB::~MemoryManagementUnitPOMTLB()
{
    for (auto &pom : m_pom_tlbs)
        delete pom.tlb;
    delete m_log;
}

bool MemoryManagementUnitPOMTLB::lookupBeyondTLBs(
    IntPtr address, IntPtr eip, bool instruction,
    Core::lock_signal_t lock, bool modeled, bool count,
    SubsecondTime &latency, IntPtr &ppn, int &page_size)
{
    SubsecondTime now = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);
    if (count)
        m_stats.lookups++;

    // Every POM-TLB is probed at once: one memory access each, of the line
    // its set starts at.  A hit ends the lookup at that access's latency;
    // when all miss, the walk starts after the slowest one.
    bool hit = false;
    SubsecondTime slowest = SubsecondTime::Zero();
    for (auto &pom : m_pom_tlbs)
    {
        CacheBlockInfo *entry = pom.tlb->lookup(address, now, count, lock, eip, modeled, count, NULL, instruction);

        IntPtr tag;
        UInt32 set_index;
        pom.tlb->getCache().splitAddressTLB(address, tag, set_index, pom.page_size);

        translationPacket packet;
        packet.eip = eip;
        packet.instruction = instruction;
        packet.lock_signal = lock;
        packet.modeled = modeled;
        packet.count = count;
        packet.type = instruction ? CacheBlockInfo::block_type_t::PAGE_TABLE_INSTRUCTION
                                  : CacheBlockInfo::block_type_t::PAGE_TABLE_DATA;
        packet.address = pom.base_address + (IntPtr)set_index * pom.tlb->getAssoc() * pom.tlb->getEntrySize();

        HitWhere::where_t where = HitWhere::UNKNOWN;
        SubsecondTime access_latency = accessCache(packet, now, false, where);
        m_log->debug("POM-TLB", pom.tlb->getName().c_str(), "set", set_index, "at", SimLog::hex(packet.address),
                     entry ? "hit" : "miss", "latency", access_latency);

        if (entry != NULL && !hit)
        {
            hit = true;
            latency = access_latency;
            ppn = entry->getPPN();
            page_size = entry->getPageSize();
        }
        slowest = std::max(slowest, access_latency);
    }

    if (!hit)
        latency = slowest;
    if (count)
    {
        m_stats.software_tlb_latency += latency;
        if (hit)
            m_stats.hits++;
        else
            m_stats.misses++;
    }
    return hit;
}

void MemoryManagementUnitPOMTLB::installBeyondTLBs(
    IntPtr address, IntPtr ppn, int page_size,
    Core::lock_signal_t lock, bool count, bool instruction)
{
    SubsecondTime now = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);
    for (auto &pom : m_pom_tlbs)
    {
        if (!pom.tlb->supportsPageSize(page_size) || !pom.tlb->getAllocateOnMiss())
            continue;
        pom.tlb->allocate(address, now, count, lock, page_size, ppn, false, instruction);
        if (count)
            m_stats.installs++;
        m_log->debug("Installed VPN", SimLog::hex(address >> page_size), "PPN", SimLog::hex(ppn),
                     "in", pom.tlb->getName().c_str());
    }
}

}
