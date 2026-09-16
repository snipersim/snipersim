#include "BertiTLBPrefetcher.h"
#include "tlb_prefetcher_base.h"
#include "stats.h"
#include <cassert>
#include <iostream>

namespace ParametricDramDirectoryMSI
{

BertiTLBPrefetcher::BertiTLBPrefetcher(
    Core *_core, MemoryManagerBase *_memory_manager,
    ShmemPerfModel *_shmem_perf_model, String name,
    uint32_t region_bits,
    uint32_t current_pages_entries,
    uint32_t prev_requests_entries,
    uint32_t record_pages_entries,
    uint32_t ip_table_entries,
    uint32_t num_berti,
    uint32_t max_burst)
    : TLBPrefetcherBase(_core, _memory_manager, _shmem_perf_model, name),
      REGION_BITS(region_bits),
      REGION_SIZE(1u << region_bits),
      REGION_OFFSET_MASK((1u << region_bits) - 1),
      NUM_BERTI(num_berti),
      MAX_BURST(max_burst),
      BERTI_CTR_CONFIDENCE(2),
      CURRENT_PAGES_ENTRIES(current_pages_entries),
      PREV_REQUESTS_ENTRIES(prev_requests_entries),
      PREV_REQUESTS_MASK(prev_requests_entries - 1),
      RECORD_PAGES_ENTRIES(record_pages_entries),
      IP_TABLE_ENTRIES(ip_table_entries),
      IP_TABLE_MASK(ip_table_entries - 1)
{
    // prev_requests_entries must be power of 2
    assert((prev_requests_entries & (prev_requests_entries - 1)) == 0);
    assert((ip_table_entries & (ip_table_entries - 1)) == 0);

    PREV_REQUESTS_NULL = CURRENT_PAGES_ENTRIES;
    IP_TABLE_NULL = RECORD_PAGES_ENTRIES;

    // Init current pages table
    current_pages_table.resize(CURRENT_PAGES_ENTRIES);
    for (uint32_t i = 0; i < CURRENT_PAGES_ENTRIES; i++) {
        current_pages_table[i].region_addr = 0;
        current_pages_table[i].ip = 0;
        current_pages_table[i].u_vector = 0;
        current_pages_table[i].first_offset = 0;
        current_pages_table[i].berti.resize(NUM_BERTI, 0);
        current_pages_table[i].berti_ctr.resize(NUM_BERTI, 0);
        current_pages_table[i].last_burst = 0;
        current_pages_table[i].lru = i;
    }

    // Init prev requests table
    prev_requests_table.resize(PREV_REQUESTS_ENTRIES);
    prev_requests_head = 0;
    for (uint32_t i = 0; i < PREV_REQUESTS_ENTRIES; i++) {
        prev_requests_table[i].region_pointer = PREV_REQUESTS_NULL;
        prev_requests_table[i].offset = 0;
    }

    // Init record pages table
    record_pages_table.resize(RECORD_PAGES_ENTRIES);
    for (uint32_t i = 0; i < RECORD_PAGES_ENTRIES; i++) {
        record_pages_table[i].region_addr = 0;
        record_pages_table[i].u_vector = 0;
        record_pages_table[i].first_offset = 0;
        record_pages_table[i].berti = 0;
        record_pages_table[i].lru = i;
    }

    // Init IP table
    ip_table.resize(IP_TABLE_ENTRIES, IP_TABLE_NULL);

    // Stats
    stats.prefetch_attempts = 0;
    stats.successful_prefetches = 0;
    stats.failed_prefetches = 0;
    stats.strides_learned = 0;
    stats.predictions_from_record = 0;
    stats.predictions_from_current = 0;

    registerStatsMetric("tlb_berti", _core->getId(), "prefetch_attempts", &stats.prefetch_attempts);
    registerStatsMetric("tlb_berti", _core->getId(), "successful_prefetches", &stats.successful_prefetches);
    registerStatsMetric("tlb_berti", _core->getId(), "failed_prefetches", &stats.failed_prefetches);
    registerStatsMetric("tlb_berti", _core->getId(), "strides_learned", &stats.strides_learned);
    registerStatsMetric("tlb_berti", _core->getId(), "predictions_from_record", &stats.predictions_from_record);
    registerStatsMetric("tlb_berti", _core->getId(), "predictions_from_current", &stats.predictions_from_current);

    std::cout << "Berti TLB prefetcher created: region_bits=" << region_bits
              << " current_pages=" << current_pages_entries
              << " prev_requests=" << prev_requests_entries
              << " record_pages=" << record_pages_entries
              << " ip_table=" << ip_table_entries
              << " num_berti=" << num_berti
              << " max_burst=" << max_burst << std::endl;
}

// ============================================================
// Current Pages Table helpers
// ============================================================

uint64_t BertiTLBPrefetcher::getCurrentPageEntry(uint64_t region_addr)
{
    for (uint32_t i = 0; i < CURRENT_PAGES_ENTRIES; i++) {
        if (current_pages_table[i].region_addr == region_addr && current_pages_table[i].u_vector != 0)
            return i;
    }
    return CURRENT_PAGES_ENTRIES;
}

void BertiTLBPrefetcher::updateLRUCurrentPages(uint64_t index)
{
    for (uint32_t i = 0; i < CURRENT_PAGES_ENTRIES; i++) {
        if (current_pages_table[i].lru < current_pages_table[index].lru)
            current_pages_table[i].lru++;
    }
    current_pages_table[index].lru = 0;
}

uint64_t BertiTLBPrefetcher::getLRUCurrentPageEntry()
{
    uint64_t lru = CURRENT_PAGES_ENTRIES;
    for (uint32_t i = 0; i < CURRENT_PAGES_ENTRIES; i++) {
        current_pages_table[i].lru++;
        if (current_pages_table[i].lru == CURRENT_PAGES_ENTRIES) {
            current_pages_table[i].lru = 0;
            lru = i;
        }
    }
    assert(lru != CURRENT_PAGES_ENTRIES);
    return lru;
}

int BertiTLBPrefetcher::getBertiCurrentPage(uint64_t index, uint64_t &ctr)
{
    uint64_t max_score = 0;
    int b = 0;
    ctr = 0;
    for (uint32_t i = 0; i < NUM_BERTI; i++) {
        if (current_pages_table[index].berti_ctr[i] > max_score) {
            b = current_pages_table[index].berti[i];
            max_score = current_pages_table[index].berti_ctr[i];
            ctr = max_score;
        }
    }
    return b;
}

void BertiTLBPrefetcher::addCurrentPage(uint64_t index, uint64_t region_addr, uint64_t ip, uint64_t offset)
{
    current_pages_table[index].region_addr = region_addr;
    current_pages_table[index].ip = ip;
    current_pages_table[index].u_vector = (uint64_t)1 << offset;
    current_pages_table[index].first_offset = offset;
    for (uint32_t i = 0; i < NUM_BERTI; i++) {
        current_pages_table[index].berti[i] = 0;
        current_pages_table[index].berti_ctr[i] = 0;
    }
    current_pages_table[index].last_burst = 0;
}

uint64_t BertiTLBPrefetcher::updateDemandCurrentPage(uint64_t index, uint64_t offset)
{
    current_pages_table[index].u_vector |= (uint64_t)1 << offset;
    updateLRUCurrentPages(index);
    return current_pages_table[index].ip;
}

void BertiTLBPrefetcher::addBertiCurrentPage(uint64_t index, int b)
{
    if (b == 0) return;
    for (uint32_t i = 0; i < NUM_BERTI; i++) {
        if (current_pages_table[index].berti_ctr[i] == 0) {
            current_pages_table[index].berti[i] = b;
            current_pages_table[index].berti_ctr[i] = 1;
            break;
        } else if (current_pages_table[index].berti[i] == b) {
            current_pages_table[index].berti_ctr[i]++;
            break;
        }
    }
    updateLRUCurrentPages(index);
    stats.strides_learned++;
}

bool BertiTLBPrefetcher::requestedOffsetCurrentPage(uint64_t index, uint64_t offset)
{
    return current_pages_table[index].u_vector & ((uint64_t)1 << offset);
}

void BertiTLBPrefetcher::removeCurrentPageEntry(uint64_t index)
{
    current_pages_table[index].region_addr = 0;
    current_pages_table[index].u_vector = 0;
}

// ============================================================
// Previous Requests Table helpers
// ============================================================

uint64_t BertiTLBPrefetcher::findPrevRequest(uint64_t pointer, uint64_t offset)
{
    for (uint32_t i = 0; i < PREV_REQUESTS_ENTRIES; i++) {
        if (prev_requests_table[i].region_pointer == pointer &&
            prev_requests_table[i].offset == offset)
            return i;
    }
    return PREV_REQUESTS_ENTRIES;
}

void BertiTLBPrefetcher::addPrevRequest(uint64_t pointer, uint64_t offset)
{
    if (findPrevRequest(pointer, offset) != PREV_REQUESTS_ENTRIES)
        return; // coalesce
    prev_requests_table[prev_requests_head].region_pointer = pointer;
    prev_requests_table[prev_requests_head].offset = offset;
    prev_requests_head = (prev_requests_head + 1) & PREV_REQUESTS_MASK;
}

void BertiTLBPrefetcher::resetPointerPrevRequests(uint64_t pointer)
{
    for (uint32_t i = 0; i < PREV_REQUESTS_ENTRIES; i++) {
        if (prev_requests_table[i].region_pointer == pointer)
            prev_requests_table[i].region_pointer = PREV_REQUESTS_NULL;
    }
}

// Simplified stride learning: find most recent previous access to same region
// and compute stride = current_offset - prev_offset
int BertiTLBPrefetcher::getStridePrevRequests(uint64_t pointer, uint64_t offset)
{
    // Walk backwards from head to find most recent access to this region
    for (uint32_t n = 0; n < PREV_REQUESTS_ENTRIES; n++) {
        uint64_t i = (prev_requests_head + PREV_REQUESTS_ENTRIES - 1 - n) & PREV_REQUESTS_MASK;
        if (prev_requests_table[i].region_pointer == pointer) {
            int stride;
            if (offset > prev_requests_table[i].offset)
                stride = (int)(offset - prev_requests_table[i].offset);
            else {
                stride = (int)(prev_requests_table[i].offset - offset);
                stride *= -1;
            }
            return stride;
        }
    }
    return 0;
}

// ============================================================
// Record Pages Table helpers
// ============================================================

uint64_t BertiTLBPrefetcher::getLRURecordPageEntry()
{
    uint64_t lru = RECORD_PAGES_ENTRIES;
    for (uint32_t i = 0; i < RECORD_PAGES_ENTRIES; i++) {
        record_pages_table[i].lru++;
        if (record_pages_table[i].lru == RECORD_PAGES_ENTRIES) {
            record_pages_table[i].lru = 0;
            lru = i;
        }
    }
    assert(lru != RECORD_PAGES_ENTRIES);
    return lru;
}

void BertiTLBPrefetcher::updateLRURecordPages(uint64_t index)
{
    for (uint32_t i = 0; i < RECORD_PAGES_ENTRIES; i++) {
        if (record_pages_table[i].lru < record_pages_table[index].lru)
            record_pages_table[i].lru++;
    }
    record_pages_table[index].lru = 0;
}

void BertiTLBPrefetcher::addRecordPage(uint64_t index, uint64_t region_addr, uint64_t u_vector, uint64_t first_offset, int berti)
{
    record_pages_table[index].region_addr = region_addr;
    record_pages_table[index].u_vector = u_vector;
    record_pages_table[index].first_offset = first_offset;
    record_pages_table[index].berti = berti;
    updateLRURecordPages(index);
}

uint64_t BertiTLBPrefetcher::getRecordPageEntry(uint64_t region_addr, uint64_t first_offset)
{
    for (uint32_t i = 0; i < RECORD_PAGES_ENTRIES; i++) {
        if (record_pages_table[i].region_addr == region_addr &&
            record_pages_table[i].first_offset == first_offset)
            return i;
    }
    return RECORD_PAGES_ENTRIES;
}

uint64_t BertiTLBPrefetcher::getRecordPageEntry(uint64_t region_addr)
{
    for (uint32_t i = 0; i < RECORD_PAGES_ENTRIES; i++) {
        if (record_pages_table[i].region_addr == region_addr)
            return i;
    }
    return RECORD_PAGES_ENTRIES;
}

void BertiTLBPrefetcher::copyRecordPageEntries(uint64_t from, uint64_t to)
{
    record_pages_table[to].region_addr = record_pages_table[from].region_addr;
    record_pages_table[to].u_vector = record_pages_table[from].u_vector;
    record_pages_table[to].first_offset = record_pages_table[from].first_offset;
    record_pages_table[to].berti = record_pages_table[from].berti;
    updateLRURecordPages(to);
}

void BertiTLBPrefetcher::recordCurrentPage(uint64_t index_current)
{
    if (current_pages_table[index_current].u_vector) {
        uint64_t record_index = ip_table[current_pages_table[index_current].ip & IP_TABLE_MASK];
        if (record_index >= RECORD_PAGES_ENTRIES) return;
        uint64_t confidence;
        addRecordPage(record_index,
                      current_pages_table[index_current].region_addr,
                      current_pages_table[index_current].u_vector,
                      current_pages_table[index_current].first_offset,
                      getBertiCurrentPage(index_current, confidence));
    }
}

// ============================================================
// Main prefetch logic
// ============================================================

std::vector<query_entry> BertiTLBPrefetcher::performPrefetch(
    IntPtr address, IntPtr eip, Core::lock_signal_t lock,
    bool modeled, bool count, PageTable *pt,
    bool instruction, bool tlb_hit, bool pq_hit, int page_size)
{
    std::vector<query_entry> result;

    IntPtr VPN = address >> 12;
    uint64_t region_addr = VPN >> REGION_BITS;
    uint64_t offset = VPN & REGION_OFFSET_MASK;
    uint64_t ip = (uint64_t)eip;

    // Find region in current pages table
    uint64_t index = getCurrentPageEntry(region_addr);

    // If already requested this offset, nothing to do
    if (index < CURRENT_PAGES_ENTRIES && requestedOffsetCurrentPage(index, offset))
        return result;

    if (index < CURRENT_PAGES_ENTRIES) {
        // Region found: update u_vector, learn stride
        uint64_t first_ip = updateDemandCurrentPage(index, offset);

        // Learn stride from previous access to this region
        int stride = getStridePrevRequests(index, offset);
        if (stride != 0) {
            addBertiCurrentPage(index, stride);
        }

        // Group IPs: if different IP, point to same record
        if (first_ip != ip) {
            uint64_t first_ptr = ip_table[first_ip & IP_TABLE_MASK];
            if (first_ptr < RECORD_PAGES_ENTRIES)
                ip_table[ip & IP_TABLE_MASK] = first_ptr;
        }
    } else {
        // Region not found: evict LRU, record victim, add new
        uint64_t victim_index = getLRUCurrentPageEntry();
        resetPointerPrevRequests(victim_index);
        recordCurrentPage(victim_index);

        index = victim_index;
        addCurrentPage(index, region_addr, ip & IP_TABLE_MASK, offset);

        // Set up IP table pointer
        uint64_t index_record = getRecordPageEntry(region_addr, offset);
        if (ip_table[ip & IP_TABLE_MASK] == IP_TABLE_NULL) {
            if (index_record == RECORD_PAGES_ENTRIES) {
                uint64_t new_pointer = getLRURecordPageEntry();
                ip_table[ip & IP_TABLE_MASK] = new_pointer;
            } else {
                ip_table[ip & IP_TABLE_MASK] = index_record;
            }
        } else if (ip_table[ip & IP_TABLE_MASK] != index_record) {
            uint64_t new_pointer = getLRURecordPageEntry();
            copyRecordPageEntries(ip_table[ip & IP_TABLE_MASK], new_pointer);
            ip_table[ip & IP_TABLE_MASK] = new_pointer;
        }
    }

    // Record this access
    addPrevRequest(index, offset);

    // ---- PREDICT ----
    uint64_t u_vector = 0;
    uint64_t first_offset = current_pages_table[index].first_offset;
    int b = 0;
    bool recorded = false;

    uint64_t ip_pointer = ip_table[ip & IP_TABLE_MASK];
    uint64_t pgo_pointer = getRecordPageEntry(region_addr, first_offset);
    uint64_t pg_pointer = getRecordPageEntry(region_addr);
    uint64_t berti_confidence = 0;
    int current_berti = getBertiCurrentPage(index, berti_confidence);
    uint64_t match_confidence = 0;

    // Priority 1: exact page+first_offset match in record
    if (pgo_pointer != RECORD_PAGES_ENTRIES &&
        (record_pages_table[pgo_pointer].u_vector | current_pages_table[index].u_vector) == record_pages_table[pgo_pointer].u_vector) {
        u_vector = record_pages_table[pgo_pointer].u_vector;
        b = record_pages_table[pgo_pointer].berti;
        match_confidence = 1;
        recorded = true;
        stats.predictions_from_record++;
    }
    // Priority 2: IP+first_offset match in record
    else if (ip_pointer < RECORD_PAGES_ENTRIES &&
             record_pages_table[ip_pointer].first_offset == first_offset &&
             (record_pages_table[ip_pointer].u_vector | current_pages_table[index].u_vector) == record_pages_table[ip_pointer].u_vector) {
        u_vector = record_pages_table[ip_pointer].u_vector;
        b = record_pages_table[ip_pointer].berti;
        match_confidence = 1;
        recorded = true;
        stats.predictions_from_record++;
    }
    // Priority 3: current berti with sufficient confidence
    else if (current_berti != 0 && berti_confidence >= BERTI_CTR_CONFIDENCE) {
        u_vector = current_pages_table[index].u_vector;
        b = current_berti;
        stats.predictions_from_current++;
    }
    // Priority 4: page match in record (no first_offset)
    else if (pg_pointer != RECORD_PAGES_ENTRIES) {
        u_vector = record_pages_table[pg_pointer].u_vector;
        b = record_pages_table[pg_pointer].berti;
        recorded = true;
        stats.predictions_from_record++;
    }
    // Priority 5: IP match in record
    else if (ip_pointer < RECORD_PAGES_ENTRIES && record_pages_table[ip_pointer].u_vector) {
        u_vector = record_pages_table[ip_pointer].u_vector;
        b = record_pages_table[ip_pointer].berti;
        recorded = true;
        stats.predictions_from_record++;
    }

    // ---- Issue burst prefetches ----
    if (b != 0 && (first_offset == offset || current_pages_table[index].last_burst != 0)) {
        uint64_t first_burst;
        if (current_pages_table[index].last_burst != 0) {
            first_burst = current_pages_table[index].last_burst;
            current_pages_table[index].last_burst = 0;
        } else if (b > 0) {
            first_burst = offset + 1;
        } else {
            first_burst = offset - 1;
        }

        if (recorded && match_confidence) {
            uint32_t bursts = 0;
            if (b > 0) {
                for (uint64_t i = first_burst; i < offset + (uint64_t)b && i < REGION_SIZE; i++) {
                    if (!(((uint64_t)1 << i) & u_vector)) continue;
                    if (requestedOffsetCurrentPage(index, i)) continue;
                    if (bursts >= MAX_BURST) {
                        current_pages_table[index].last_burst = i;
                        break;
                    }
                    IntPtr pf_vpn = (region_addr << REGION_BITS) | i;
                    stats.prefetch_attempts++;
                    query_entry qe = PTWTransparent(pf_vpn << 12, eip, lock, modeled, count, pt);
                    if (qe.ppn != 0) {
                        result.push_back(qe);
                        stats.successful_prefetches++;
                        bursts++;
                    } else {
                        stats.failed_prefetches++;
                    }
                }
            } else { // b < 0
                for (int64_t i = (int64_t)first_burst; i > (int64_t)offset + b && i >= 0; i--) {
                    if (!(((uint64_t)1 << i) & u_vector)) continue;
                    if (requestedOffsetCurrentPage(index, (uint64_t)i)) continue;
                    if (bursts >= MAX_BURST) {
                        current_pages_table[index].last_burst = (uint64_t)i;
                        break;
                    }
                    IntPtr pf_vpn = (region_addr << REGION_BITS) | (uint64_t)i;
                    stats.prefetch_attempts++;
                    query_entry qe = PTWTransparent(pf_vpn << 12, eip, lock, modeled, count, pt);
                    if (qe.ppn != 0) {
                        result.push_back(qe);
                        stats.successful_prefetches++;
                        bursts++;
                    } else {
                        stats.failed_prefetches++;
                    }
                }
            }
        }
    }

    // Single stride prefetch (always, if b != 0)
    if (b != 0) {
        int64_t pf_offset = (int64_t)offset + b;
        if (pf_offset >= 0 && pf_offset < (int64_t)REGION_SIZE) {
            if (!requestedOffsetCurrentPage(index, (uint64_t)pf_offset) &&
                (!match_confidence || (((uint64_t)1 << pf_offset) & u_vector))) {
                IntPtr pf_vpn = (region_addr << REGION_BITS) | (uint64_t)pf_offset;
                stats.prefetch_attempts++;
                query_entry qe = PTWTransparent(pf_vpn << 12, eip, lock, modeled, count, pt);
                if (qe.ppn != 0) {
                    result.push_back(qe);
                    stats.successful_prefetches++;
                } else {
                    stats.failed_prefetches++;
                }
            }
        }
    }

    return result;
}

} // namespace ParametricDramDirectoryMSI
