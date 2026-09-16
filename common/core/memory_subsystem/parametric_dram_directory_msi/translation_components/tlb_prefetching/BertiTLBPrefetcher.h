#pragma once
#include "tlb_prefetcher_base.h"
#include "stats.h"
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace ParametricDramDirectoryMSI
{

// Berti TLB Prefetcher
// Adapted from the cache-level Berti prefetcher (Bera et al.)
// Operates on VPN regions instead of cache-line pages.
// A "region" is a group of consecutive VPNs (default 64).
// Learns dominant VPN stride per region and uses recorded patterns to prefetch.
// Latency-aware feedback is skipped; stride is learned from consecutive accesses.

class BertiTLBPrefetcher : public TLBPrefetcherBase
{
public:
    BertiTLBPrefetcher(Core *_core, MemoryManagerBase *_memory_manager,
                       ShmemPerfModel *_shmem_perf_model, String name,
                       uint32_t region_bits,
                       uint32_t current_pages_entries,
                       uint32_t prev_requests_entries,
                       uint32_t record_pages_entries,
                       uint32_t ip_table_entries,
                       uint32_t num_berti,
                       uint32_t max_burst);

    std::vector<query_entry> performPrefetch(IntPtr address, IntPtr eip,
        Core::lock_signal_t lock, bool modeled, bool count,
        PageTable *pt, bool instruction = false,
        bool tlb_hit = false, bool pq_hit = false, int page_size = 12) override;

private:
    // Parameters
    const uint32_t REGION_BITS;
    const uint32_t REGION_SIZE;        // 1 << REGION_BITS
    const uint64_t REGION_OFFSET_MASK; // REGION_SIZE - 1
    const uint32_t NUM_BERTI;
    const uint32_t MAX_BURST;
    const uint32_t BERTI_CTR_CONFIDENCE;

    // ---- Current pages (regions) table ----
    struct CurrentPageEntry {
        uint64_t region_addr;
        uint64_t ip;
        uint64_t u_vector;        // bitmap of accessed offsets
        uint64_t first_offset;
        std::vector<int>      berti;      // learned strides
        std::vector<unsigned> berti_ctr;  // confidence counters
        uint64_t last_burst;
        uint64_t lru;
    };
    std::vector<CurrentPageEntry> current_pages_table;
    const uint32_t CURRENT_PAGES_ENTRIES;

    uint64_t getCurrentPageEntry(uint64_t region_addr);
    void     updateLRUCurrentPages(uint64_t index);
    uint64_t getLRUCurrentPageEntry();
    int      getBertiCurrentPage(uint64_t index, uint64_t &ctr);
    void     addCurrentPage(uint64_t index, uint64_t region_addr, uint64_t ip, uint64_t offset);
    uint64_t updateDemandCurrentPage(uint64_t index, uint64_t offset);
    void     addBertiCurrentPage(uint64_t index, int b);
    bool     requestedOffsetCurrentPage(uint64_t index, uint64_t offset);
    void     removeCurrentPageEntry(uint64_t index);

    // ---- Previous requests table (circular buffer) ----
    struct PrevRequestEntry {
        uint64_t region_pointer;  // index into current_pages_table
        uint64_t offset;
    };
    std::vector<PrevRequestEntry> prev_requests_table;
    uint64_t prev_requests_head;
    const uint32_t PREV_REQUESTS_ENTRIES;
    const uint64_t PREV_REQUESTS_MASK;
    uint64_t PREV_REQUESTS_NULL;

    uint64_t findPrevRequest(uint64_t pointer, uint64_t offset);
    void     addPrevRequest(uint64_t pointer, uint64_t offset);
    void     resetPointerPrevRequests(uint64_t pointer);
    int      getStridePrevRequests(uint64_t pointer, uint64_t offset);

    // ---- Record pages table ----
    struct RecordPageEntry {
        uint64_t region_addr;
        uint64_t u_vector;
        uint64_t first_offset;
        int      berti;
        uint64_t lru;
    };
    std::vector<RecordPageEntry> record_pages_table;
    const uint32_t RECORD_PAGES_ENTRIES;

    uint64_t getLRURecordPageEntry();
    void     updateLRURecordPages(uint64_t index);
    void     addRecordPage(uint64_t index, uint64_t region_addr, uint64_t u_vector, uint64_t first_offset, int berti);
    uint64_t getRecordPageEntry(uint64_t region_addr, uint64_t first_offset);
    uint64_t getRecordPageEntry(uint64_t region_addr);
    void     copyRecordPageEntries(uint64_t from, uint64_t to);

    // ---- IP table ----
    std::vector<uint64_t> ip_table;
    const uint32_t IP_TABLE_ENTRIES;
    const uint64_t IP_TABLE_MASK;
    uint64_t IP_TABLE_NULL;

    // Record current page to record table
    void recordCurrentPage(uint64_t index_current);

    // Stats
    struct {
        UInt64 prefetch_attempts;
        UInt64 successful_prefetches;
        UInt64 failed_prefetches;
        UInt64 strides_learned;
        UInt64 predictions_from_record;
        UInt64 predictions_from_current;
    } stats;
};

} // namespace ParametricDramDirectoryMSI
