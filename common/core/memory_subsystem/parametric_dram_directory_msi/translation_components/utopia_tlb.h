#ifndef UTOPIA_TLB_H
#define UTOPIA_TLB_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include "sim_log.h"
#include <vector>
#include <cstdint>

namespace ParametricDramDirectoryMSI
{

/**
 * @brief The Utopia TLB (UTLB): a compact TLB of RestSeg translations.
 *
 * A RestSeg page's physical address follows from its set and way, so the
 * UTLB stores (VPN tag, RestSeg id, way, page size) rather than a PPN --
 * about a third of a conventional entry's bits, so the same SRAM holds
 * roughly 2.8 times the entries.  On a hit the MMU recomputes the physical
 * address from the RestSeg; a miss leaves the RestSeg walk to answer.
 * Set-associative with LRU, one page size bit per entry (4KB or 2MB).
 */
struct UTLBEntry {
    uint64_t vpn_tag;
    uint8_t  seg_id;
    uint8_t  way_idx;
    uint8_t  page_size_bit;   // 0: 4KB, 1: 2MB
    bool     valid;

    UTLBEntry() : vpn_tag(0), seg_id(0), way_idx(0), page_size_bit(0), valid(false) {}

    int getPageSizeBits() const { return page_size_bit ? 21 : 12; }

    // Bits of one entry: tag (48 - 12 - set index bits), segment id, way, valid, page size
    static int getBitsPerEntry(int num_sets, int num_restsegs, int max_ways) {
        auto bits_for = [](int n) { int b = 0; while (n > 1) { n >>= 1; b++; } return b; };
        return (48 - 12 - bits_for(num_sets)) + bits_for(num_restsegs) + bits_for(max_ways) + 1 + 1;
    }
};

struct UTLBLookupResult {
    bool     hit;
    uint8_t  seg_id;
    uint8_t  way_idx;
    int      page_size;   // bits, valid on a hit

    UTLBLookupResult() : hit(false), seg_id(0), way_idx(0), page_size(0) {}
    UTLBLookupResult(bool hit, uint8_t seg_id, uint8_t way_idx, int page_size)
        : hit(hit), seg_id(seg_id), way_idx(way_idx), page_size(page_size) {}
};

class UtopiaTLB {
private:
    String m_name;
    core_id_t m_core_id;

    UInt32 m_num_entries;
    UInt32 m_associativity;
    UInt32 m_num_sets;
    UInt32 m_set_index_bits;

    std::vector<std::vector<UTLBEntry>> m_sets;
    std::vector<std::vector<uint8_t>> m_lru_stack;   // per set, MRU first

    SubsecondTime m_access_latency;

    struct {
        UInt64 accesses;
        UInt64 hits;
        UInt64 misses;
        UInt64 evictions;
        UInt64 allocations;
    } m_stats;

    SimLog* m_log;

    UInt64 getSetIndex(IntPtr address, int page_size_bits) const;
    UInt64 getVPNTag(IntPtr address, int page_size_bits) const;
    void updateLRU(UInt32 set_idx, UInt32 way_idx);
    UInt32 getLRUVictim(UInt32 set_idx) const;

public:
    UtopiaTLB(String name, core_id_t core_id, UInt32 num_entries, UInt32 associativity, SubsecondTime access_latency);
    ~UtopiaTLB();

    // Look a page up at one page size; count says whether it is a counted access
    UTLBLookupResult lookup(IntPtr address, int page_size_bits, bool count = true);

    // Install (seg_id, way) for the page; returns whether an entry was evicted
    bool allocate(IntPtr address, int page_size_bits, uint8_t seg_id, uint8_t way_idx, bool count = true);

    bool invalidate(IntPtr address, int page_size_bits);
    void flush();

    String getName() const { return m_name; }
    SubsecondTime getLatency() const { return m_access_latency; }
    UInt32 getNumEntries() const { return m_num_entries; }
    UInt32 getAssociativity() const { return m_associativity; }
    UInt32 getNumSets() const { return m_num_sets; }

    struct Stats {
        UInt64 accesses, hits, misses, evictions, allocations;
    };
    Stats getStats() const {
        return Stats{m_stats.accesses, m_stats.hits, m_stats.misses, m_stats.evictions, m_stats.allocations};
    }

    int getBitsPerEntry() const { return UTLBEntry::getBitsPerEntry(m_num_sets, 2, 16); }
    float getCapacityRatio(int standard_tlb_bits_per_entry = 96) const {
        return (float)standard_tlb_bits_per_entry / getBitsPerEntry();
    }
};

} // namespace ParametricDramDirectoryMSI

#endif // UTOPIA_TLB_H
