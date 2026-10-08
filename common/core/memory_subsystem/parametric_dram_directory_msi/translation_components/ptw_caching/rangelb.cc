#include "rangelb.h"
#include "stats.h"
#include "simulator.h"
#include "debug_config.h"
#include <cstdlib>
#include <cstring>

RLB::RLB(Core *core, String name, ComponentLatency latency, UInt64 entries)
    : m_latency(latency),
      m_capacity(entries),
      m_mru(-1),
      m_core(core),
      m_name(name)
{
    m_log = new SimLog("RLB", core->getId(), DEBUG_RLB);
    m_entries.reserve(entries);
    memset(&stats, 0, sizeof(stats));
    registerStatsMetric(name, core->getId(), "accesses", &stats.accesses);
    registerStatsMetric(name, core->getId(), "hits", &stats.hits);
    registerStatsMetric(name, core->getId(), "misses", &stats.misses);
    registerStatsMetric(name, core->getId(), "insertions", &stats.insertions);
    registerStatsMetric(name, core->getId(), "evictions", &stats.evictions);
}

RLB::~RLB()
{
    delete m_log;
}

void RLB::insert(const Entry &entry)
{
    m_log->debug("Inserting range", SimLog::hex(entry.base), "-", SimLog::hex(entry.end), "at frame", entry.ppn);
    stats.insertions++;
    if (m_entries.size() == m_capacity)
    {
        // A random entry other than the most recently hit one; with a
        // single entry there is no other.
        int victim = rand() % m_capacity;
        if (m_capacity > 1)
            while (victim == m_mru)
                victim = rand() % m_capacity;
        m_entries.erase(m_entries.begin() + victim);
        if (m_mru > victim)
            m_mru--;
        else if (m_mru == victim)
            m_mru = -1;
        stats.evictions++;
    }
    m_entries.push_back(entry);
}

std::pair<bool, RLB::Entry> RLB::lookup(IntPtr address, bool count)
{
    if (count)
        stats.accesses++;
    for (size_t i = 0; i < m_entries.size(); i++)
    {
        const Entry &e = m_entries[i];
        if (address >= e.base && address < e.end)
        {
            if (count)
                stats.hits++;
            m_mru = i;
            m_log->debug("Hit for", SimLog::hex(address), "in", SimLog::hex(e.base), "-", SimLog::hex(e.end));
            return std::make_pair(true, e);
        }
    }
    if (count)
        stats.misses++;
    m_log->debug("Miss for", SimLog::hex(address));
    return std::make_pair(false, Entry{0, 0, 0});
}
