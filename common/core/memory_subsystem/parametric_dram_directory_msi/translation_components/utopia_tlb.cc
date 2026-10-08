#include "utopia_tlb.h"
#include "stats.h"
#include "simulator.h"
#include "config.hpp"
#include "debug_config.h"
#include <algorithm>
#include <iostream>

namespace ParametricDramDirectoryMSI
{

UtopiaTLB::UtopiaTLB(String name, core_id_t core_id, UInt32 num_entries, UInt32 associativity, SubsecondTime access_latency)
    : m_name(name)
    , m_core_id(core_id)
    , m_num_entries(num_entries)
    , m_associativity(associativity)
    , m_num_sets(num_entries / associativity)
    , m_access_latency(access_latency)
{
    m_set_index_bits = 0;
    for (UInt32 t = m_num_sets; t > 1; t >>= 1)
        m_set_index_bits++;

    m_sets.resize(m_num_sets);
    m_lru_stack.resize(m_num_sets);
    for (UInt32 i = 0; i < m_num_sets; i++) {
        m_sets[i].resize(m_associativity);
        m_lru_stack[i].resize(m_associativity);
        for (UInt32 j = 0; j < m_associativity; j++)
            m_lru_stack[i][j] = j;
    }

    m_stats.accesses = 0;
    m_stats.hits = 0;
    m_stats.misses = 0;
    m_stats.evictions = 0;
    m_stats.allocations = 0;
    registerStatsMetric(name, core_id, "accesses", &m_stats.accesses);
    registerStatsMetric(name, core_id, "hits", &m_stats.hits);
    registerStatsMetric(name, core_id, "misses", &m_stats.misses);
    registerStatsMetric(name, core_id, "evictions", &m_stats.evictions);
    registerStatsMetric(name, core_id, "allocations", &m_stats.allocations);

    m_log = new SimLog(name.c_str(), core_id, DEBUG_UTOPIA);

    std::cout << "[UTLB] " << m_name << " core " << m_core_id << ": " << m_num_entries << " entries, "
              << m_associativity << "-way, " << m_num_sets << " sets, " << getBitsPerEntry()
              << " bits/entry (" << getCapacityRatio() << "x a 96-bit TLB entry), latency "
              << m_access_latency.getNS() << " ns" << std::endl;
}

UtopiaTLB::~UtopiaTLB()
{
    delete m_log;
}

UInt64 UtopiaTLB::getSetIndex(IntPtr address, int page_size_bits) const
{
    return (address >> page_size_bits) & ((1ULL << m_set_index_bits) - 1);
}

UInt64 UtopiaTLB::getVPNTag(IntPtr address, int page_size_bits) const
{
    return (address >> page_size_bits) >> m_set_index_bits;
}

void UtopiaTLB::updateLRU(UInt32 set_idx, UInt32 way_idx)
{
    auto& stack = m_lru_stack[set_idx];
    auto it = std::find(stack.begin(), stack.end(), (uint8_t)way_idx);
    if (it != stack.end())
        stack.erase(it);
    stack.insert(stack.begin(), (uint8_t)way_idx);
}

UInt32 UtopiaTLB::getLRUVictim(UInt32 set_idx) const
{
    for (UInt32 i = 0; i < m_associativity; i++)
        if (!m_sets[set_idx][i].valid)
            return i;
    return m_lru_stack[set_idx].back();
}

UTLBLookupResult UtopiaTLB::lookup(IntPtr address, int page_size_bits, bool count)
{
    if (count)
        m_stats.accesses++;

    UInt64 set_idx = getSetIndex(address, page_size_bits);
    UInt64 vpn_tag = getVPNTag(address, page_size_bits);
    uint8_t page_size_bit = (page_size_bits == 21) ? 1 : 0;

    for (UInt32 way = 0; way < m_associativity; way++) {
        const UTLBEntry& entry = m_sets[set_idx][way];
        if (entry.valid && entry.vpn_tag == vpn_tag && entry.page_size_bit == page_size_bit) {
            if (count)
                m_stats.hits++;
            updateLRU(set_idx, way);
            m_log->debug("UTLB hit: addr=", SimLog::hex(address), " seg_id=", (int)entry.seg_id, " way_idx=", (int)entry.way_idx);
            return UTLBLookupResult(true, entry.seg_id, entry.way_idx, entry.getPageSizeBits());
        }
    }

    if (count)
        m_stats.misses++;
    return UTLBLookupResult();
}

bool UtopiaTLB::allocate(IntPtr address, int page_size_bits, uint8_t seg_id, uint8_t way_idx, bool count)
{
    UInt64 set_idx = getSetIndex(address, page_size_bits);
    UInt64 vpn_tag = getVPNTag(address, page_size_bits);
    uint8_t page_size_bit = (page_size_bits == 21) ? 1 : 0;

    // Already present: update in place
    for (UInt32 way = 0; way < m_associativity; way++) {
        UTLBEntry& entry = m_sets[set_idx][way];
        if (entry.valid && entry.vpn_tag == vpn_tag && entry.page_size_bit == page_size_bit) {
            entry.seg_id = seg_id;
            entry.way_idx = way_idx;
            updateLRU(set_idx, way);
            return false;
        }
    }

    if (count)
        m_stats.allocations++;

    UInt32 victim_way = getLRUVictim(set_idx);
    bool eviction = m_sets[set_idx][victim_way].valid;
    if (eviction && count)
        m_stats.evictions++;

    UTLBEntry& entry = m_sets[set_idx][victim_way];
    entry.vpn_tag = vpn_tag;
    entry.seg_id = seg_id;
    entry.way_idx = way_idx;
    entry.page_size_bit = page_size_bit;
    entry.valid = true;
    updateLRU(set_idx, victim_way);

    m_log->debug("UTLB allocate: addr=", SimLog::hex(address), " seg_id=", (int)seg_id, " way_idx=", (int)way_idx,
                 " victim_way=", victim_way, " evicted=", eviction);
    return eviction;
}

bool UtopiaTLB::invalidate(IntPtr address, int page_size_bits)
{
    UInt64 set_idx = getSetIndex(address, page_size_bits);
    UInt64 vpn_tag = getVPNTag(address, page_size_bits);
    uint8_t page_size_bit = (page_size_bits == 21) ? 1 : 0;

    for (UInt32 way = 0; way < m_associativity; way++) {
        UTLBEntry& entry = m_sets[set_idx][way];
        if (entry.valid && entry.vpn_tag == vpn_tag && entry.page_size_bit == page_size_bit) {
            entry.valid = false;
            return true;
        }
    }
    return false;
}

void UtopiaTLB::flush()
{
    for (UInt32 set = 0; set < m_num_sets; set++) {
        for (UInt32 way = 0; way < m_associativity; way++)
            m_sets[set][way].valid = false;
        for (UInt32 j = 0; j < m_associativity; j++)
            m_lru_stack[set][j] = j;
    }
}

} // namespace ParametricDramDirectoryMSI
