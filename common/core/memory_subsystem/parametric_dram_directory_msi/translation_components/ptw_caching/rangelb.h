#pragma once
// ============================================================================
// Range Lookaside Buffer: the hardware cache of range translations of
// Redundant Memory Mappings [Karakostas et al., ISCA 2015]
// ============================================================================
//
// A range translation maps a contiguous virtual range [base, end) to a
// contiguous physical one at a fixed offset, so one entry covers what the
// TLBs would hold as thousands of pages.  The RLB is fully associative: a
// lookup scans its entries for the one whose range holds the address; a
// miss is filled from the range table by the MMU, evicting a random entry
// that is not the most recently hit one.
// ============================================================================

#include <vector>
#include "fixed_types.h"
#include "core.h"
#include "sim_log.h"
#include "memory_management/misc/vma.h"

class RLB
{
public:
    // A range as the RLB keeps it: byte addresses of the range's base and
    // end (exclusive), and the frame number its base maps to.
    struct Entry
    {
        IntPtr base;
        IntPtr end;
        IntPtr ppn;
    };

    RLB(Core *core, String name, ComponentLatency latency, UInt64 entries);
    ~RLB();

    void insert(const Entry &entry);
    // The entry holding the address, if any.
    std::pair<bool, Entry> lookup(IntPtr address, bool count);
    SubsecondTime getLatency() { return m_latency.getLatency(); }
    UInt64 size() const { return m_entries.size(); }

private:
    ComponentLatency m_latency;
    UInt64 m_capacity;
    int m_mru;                       // index of the last entry hit, -1 when none
    Core *m_core;
    String m_name;
    std::vector<Entry> m_entries;
    SimLog *m_log;

    struct
    {
        UInt64 accesses;
        UInt64 hits;
        UInt64 misses;
        UInt64 insertions;
        UInt64 evictions;
    } stats;
};
