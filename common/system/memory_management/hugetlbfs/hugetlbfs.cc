/**
 * @file hugetlbfs.cc
 * @brief HugeTLBfs Implementation - Huge Page Filesystem OS Service
 *
 * Implementation of the hugetlbfs service that manages pre-allocated
 * huge page pools for applications requiring large page support.
 */

#include "hugetlbfs.h"
#include "simulator.h"
#include "config.hpp"
#include "stats.h"
#include <cstring>
#include <iostream>
#include <cassert>

// #define DEBUG_HUGETLBFS

// ============================================================================
// Construction / Destruction
// ============================================================================

HugeTLBfs::HugeTLBfs()
    : m_enabled(false)
    , m_nr_hugepages_2mb(0)
    , m_free_hugepages_2mb(0)
    , m_nr_hugepages_1gb(0)
    , m_free_hugepages_1gb(0)
    , m_overcommit(false)
    , m_pool_base_ppn(0)
    , m_pool_end_ppn(0)
    , m_min_vma_mb(0)
{
    // Read configuration
    m_enabled = Sim()->getCfg()->getBoolDefault("perf_model/hugetlbfs/enabled", false);

    if (!m_enabled) {
        std::cout << "[HUGETLBFS] Service disabled in configuration" << std::endl;
        return;
    }

    // Read pool sizes from config (use hasKey to check if config exists)
    if (Sim()->getCfg()->hasKey("perf_model/hugetlbfs/nr_hugepages_2mb")) {
        m_nr_hugepages_2mb = Sim()->getCfg()->getInt("perf_model/hugetlbfs/nr_hugepages_2mb");
    }
    if (Sim()->getCfg()->hasKey("perf_model/hugetlbfs/nr_hugepages_1gb")) {
        m_nr_hugepages_1gb = Sim()->getCfg()->getInt("perf_model/hugetlbfs/nr_hugepages_1gb");
        /* A pool nothing could use: faults take 2 MiB pages from the service,
           and the page tables, TLBs and walk caches model 4 KiB and 2 MiB
           pages, not 1 GiB ones */
        if (m_nr_hugepages_1gb > 0)
            LOG_PRINT_ERROR("perf_model/hugetlbfs/nr_hugepages_1gb = %lu: 1 GiB huge pages are not modelled (the page "
                            "tables map 4 KiB and 2 MiB pages); set it to 0 and use nr_hugepages_2mb.",
                            (unsigned long)m_nr_hugepages_1gb);
    }
    m_overcommit = Sim()->getCfg()->getBoolDefault("perf_model/hugetlbfs/overcommit", false);

    // The backing criterion: which of an application's VMAs the pool stands
    // behind (see the header).  Nothing in a trace says MAP_HUGETLB, so it is
    // configuration or nothing.
    m_min_vma_mb = Sim()->getCfg()->getIntDefault("perf_model/hugetlbfs/min_vma_mb", 0);
    std::string spec;
    if (Sim()->getCfg()->hasKey("perf_model/hugetlbfs/backed_ranges"))
        spec = Sim()->getCfg()->getString("perf_model/hugetlbfs/backed_ranges").c_str();
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t comma = spec.find(',', pos);
        if (comma == std::string::npos) comma = spec.size();
        std::string item = spec.substr(pos, comma - pos);
        pos = comma + 1;
        size_t dash = item.find('-');
        if (item.find_first_not_of(" \t") == std::string::npos) continue;
        if (dash == std::string::npos) {
            std::cerr << "[HUGETLBFS] backed_ranges: expected start-end, got '" << item << "'" << std::endl;
            exit(1);
        }
        IntPtr start = std::stoull(item.substr(0, dash), nullptr, 16);
        IntPtr end = std::stoull(item.substr(dash + 1), nullptr, 16);
        if (end <= start) {
            std::cerr << "[HUGETLBFS] backed_ranges: empty range '" << item << "'" << std::endl;
            exit(1);
        }
        m_backed_ranges.push_back(std::make_pair(start, end));
    }

    // Setup logging
    m_log_file_name = "hugetlbfs.log";
    m_log_file_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + m_log_file_name;
    m_log_file.open(m_log_file_name.c_str());

    std::cout << "[HUGETLBFS] Initializing HugeTLBfs service" << std::endl;
    std::cout << "[HUGETLBFS]   2MB huge pages: " << m_nr_hugepages_2mb << std::endl;
    std::cout << "[HUGETLBFS]   1GB huge pages: " << m_nr_hugepages_1gb << std::endl;
    std::cout << "[HUGETLBFS]   Overcommit: " << (m_overcommit ? "enabled" : "disabled") << std::endl;
    if (m_min_vma_mb > 0)
        std::cout << "[HUGETLBFS]   Backing VMAs of at least " << m_min_vma_mb << " MB" << std::endl;
    if (!m_backed_ranges.empty())
        std::cout << "[HUGETLBFS]   Backing " << m_backed_ranges.size() << " configured address range(s)" << std::endl;
    if (!hasBackingCriterion())
        std::cout << "[HUGETLBFS]   No backing criterion (min_vma_mb = 0, no backed_ranges): the pool backs nothing" << std::endl;

    // Initialize statistics
    memset(&m_stats, 0, sizeof(m_stats));
    registerStats();

    // Initialize the huge page pool
    initializePool();

    m_log_file << "[HUGETLBFS] Service initialized successfully" << std::endl;
}

HugeTLBfs::~HugeTLBfs()
{
    if (m_log_file.is_open()) {
        dumpPoolState();
        m_log_file.close();
    }
    std::cout << "[HUGETLBFS] Service destroyed" << std::endl;
}

// ============================================================================
// Pool Initialization
// ============================================================================

void HugeTLBfs::initializePool()
{
    // TODO: Get base physical address from memory allocator
    // For now, we use a high address range to avoid conflicts
    // In a real system, this would be reserved at boot time
    m_pool_base_ppn = 0x100000;  // Start at 4GB physical (in 4KB pages)

    IntPtr current_ppn = m_pool_base_ppn;

    // Initialize 2MB huge page pool
    m_pool_2mb.reserve(m_nr_hugepages_2mb);
    for (UInt64 i = 0; i < m_nr_hugepages_2mb; i++) {
        HugePageInfo info;
        info.base_ppn = current_ppn;
        info.app_id = -1;
        info.vaddr = 0;
        info.size = HugePageSize::SIZE_2MB;
        info.in_use = false;
        m_pool_2mb.push_back(info);
        current_ppn += HUGEPAGE_2MB_PAGES;  // Advance by 512 pages (2MB)
    }
    m_free_hugepages_2mb = m_nr_hugepages_2mb;

    // Initialize 1GB huge page pool
    m_pool_1gb.reserve(m_nr_hugepages_1gb);
    for (UInt64 i = 0; i < m_nr_hugepages_1gb; i++) {
        HugePageInfo info;
        info.base_ppn = current_ppn;
        info.app_id = -1;
        info.vaddr = 0;
        info.size = HugePageSize::SIZE_1GB;
        info.in_use = false;
        m_pool_1gb.push_back(info);
        current_ppn += HUGEPAGE_1GB_PAGES;  // Advance by 262144 pages (1GB)
    }
    m_free_hugepages_1gb = m_nr_hugepages_1gb;
    m_pool_end_ppn = current_ppn;

    m_log_file << "[HUGETLBFS] Pool initialized:" << std::endl;
    m_log_file << "[HUGETLBFS]   2MB pool: " << m_nr_hugepages_2mb << " pages, base PPN: 0x" 
               << std::hex << m_pool_base_ppn << std::dec << std::endl;
    m_log_file << "[HUGETLBFS]   1GB pool: " << m_nr_hugepages_1gb << " pages" << std::endl;
}

void HugeTLBfs::registerStats()
{
    registerStatsMetric("hugetlbfs", 0, "allocations_2mb", &m_stats.allocations_2mb);
    registerStatsMetric("hugetlbfs", 0, "allocations_1gb", &m_stats.allocations_1gb);
    registerStatsMetric("hugetlbfs", 0, "deallocations_2mb", &m_stats.deallocations_2mb);
    registerStatsMetric("hugetlbfs", 0, "deallocations_1gb", &m_stats.deallocations_1gb);
    registerStatsMetric("hugetlbfs", 0, "allocation_failures", &m_stats.allocation_failures);
    registerStatsMetric("hugetlbfs", 0, "pool_exhausted_events", &m_stats.pool_exhausted_events);
}

// ============================================================================
// Huge Page Allocation Interface
// ============================================================================

IntPtr HugeTLBfs::allocateHugePage(int app_id, IntPtr vaddr, HugePageSize size)
{
    if (!m_enabled) {
        return static_cast<IntPtr>(-1);
    }

#ifdef DEBUG_HUGETLBFS
    m_log_file << "[HUGETLBFS] Allocate request: app=" << app_id 
               << " vaddr=0x" << std::hex << vaddr << std::dec
               << " size=" << (size == HugePageSize::SIZE_2MB ? "2MB" : "1GB") << std::endl;
#endif

    // Find a free huge page
    int idx = findFreeHugePage(size);

    if (idx < 0) {
        // No free pages available
        m_stats.allocation_failures++;

        // Check if this is a pool exhausted event
        if (size == HugePageSize::SIZE_2MB && m_free_hugepages_2mb == 0) {
            m_stats.pool_exhausted_events++;
        } else if (size == HugePageSize::SIZE_1GB && m_free_hugepages_1gb == 0) {
            m_stats.pool_exhausted_events++;
        }

#ifdef DEBUG_HUGETLBFS
        m_log_file << "[HUGETLBFS] Allocation FAILED: pool exhausted" << std::endl;
#endif
        return static_cast<IntPtr>(-1);
    }

    // Allocate the page
    std::vector<HugePageInfo>& pool = (size == HugePageSize::SIZE_2MB) ? m_pool_2mb : m_pool_1gb;
    HugePageInfo& page = pool[idx];

    page.in_use = true;
    page.app_id = app_id;
    page.vaddr = vaddr;

    // Update counters
    if (size == HugePageSize::SIZE_2MB) {
        m_free_hugepages_2mb--;
        m_app_usage_2mb[app_id]++;
        m_stats.allocations_2mb++;
    } else {
        m_free_hugepages_1gb--;
        m_app_usage_1gb[app_id]++;
        m_stats.allocations_1gb++;
    }

#ifdef DEBUG_HUGETLBFS
    m_log_file << "[HUGETLBFS] Allocated: base_ppn=0x" << std::hex << page.base_ppn << std::dec
               << " to app=" << app_id << std::endl;
#endif

    return page.base_ppn;
}

bool HugeTLBfs::deallocateHugePage(IntPtr base_ppn, HugePageSize size)
{
    if (!m_enabled) {
        return false;
    }

#ifdef DEBUG_HUGETLBFS
    m_log_file << "[HUGETLBFS] Deallocate request: base_ppn=0x" << std::hex << base_ppn << std::dec
               << " size=" << (size == HugePageSize::SIZE_2MB ? "2MB" : "1GB") << std::endl;
#endif

    std::vector<HugePageInfo>& pool = (size == HugePageSize::SIZE_2MB) ? m_pool_2mb : m_pool_1gb;

    // Find the page in the pool
    for (size_t i = 0; i < pool.size(); i++) {
        if (pool[i].base_ppn == base_ppn && pool[i].in_use) {
            int app_id = pool[i].app_id;

            // Free the page
            pool[i].in_use = false;
            pool[i].app_id = -1;
            pool[i].vaddr = 0;

            // Update counters
            if (size == HugePageSize::SIZE_2MB) {
                m_free_hugepages_2mb++;
                if (m_app_usage_2mb.count(app_id) && m_app_usage_2mb[app_id] > 0) {
                    m_app_usage_2mb[app_id]--;
                }
                m_stats.deallocations_2mb++;
            } else {
                m_free_hugepages_1gb++;
                if (m_app_usage_1gb.count(app_id) && m_app_usage_1gb[app_id] > 0) {
                    m_app_usage_1gb[app_id]--;
                }
                m_stats.deallocations_1gb++;
            }

#ifdef DEBUG_HUGETLBFS
            m_log_file << "[HUGETLBFS] Deallocated: base_ppn=0x" << std::hex << base_ppn << std::dec
                       << " from app=" << app_id << std::endl;
#endif
            return true;
        }
    }

#ifdef DEBUG_HUGETLBFS
    m_log_file << "[HUGETLBFS] Deallocate FAILED: page not found" << std::endl;
#endif
    return false;
}

int HugeTLBfs::deallocateAllForApp(int app_id)
{
    if (!m_enabled) {
        return 0;
    }

    int count = 0;

    // Deallocate 2MB pages
    for (auto& page : m_pool_2mb) {
        if (page.in_use && page.app_id == app_id) {
            page.in_use = false;
            page.app_id = -1;
            page.vaddr = 0;
            m_free_hugepages_2mb++;
            m_stats.deallocations_2mb++;
            count++;
        }
    }

    // Deallocate 1GB pages
    for (auto& page : m_pool_1gb) {
        if (page.in_use && page.app_id == app_id) {
            page.in_use = false;
            page.app_id = -1;
            page.vaddr = 0;
            m_free_hugepages_1gb++;
            m_stats.deallocations_1gb++;
            count++;
        }
    }

    // Clear app usage tracking
    m_app_usage_2mb.erase(app_id);
    m_app_usage_1gb.erase(app_id);

#ifdef DEBUG_HUGETLBFS
    m_log_file << "[HUGETLBFS] Deallocated all for app=" << app_id 
               << " count=" << count << std::endl;
#endif

    return count;
}

// ============================================================================
// Backing Criterion
// ============================================================================

bool HugeTLBfs::backs(IntPtr block_base, IntPtr block_end, const std::vector<VMA>& vmas) const
{
    if (!m_enabled || !hasBackingCriterion())
        return false;

    const IntPtr min_bytes = m_min_vma_mb << 20;
    bool inside_vma = false;
    for (const auto& vma : vmas) {
        if (vma.getBase() > block_base || vma.getEnd() < block_end)
            continue;               // does not cover the whole block
        inside_vma = true;
        if (m_min_vma_mb > 0 && vma.getEnd() - vma.getBase() >= min_bytes)
            return true;
    }
    if (!inside_vma)
        return false;               // not mapped by the application at all
    for (const auto& range : m_backed_ranges) {
        if (range.first <= block_base && range.second >= block_end)
            return true;
    }
    return false;
}

// ============================================================================
// Pool Query Interface
// ============================================================================

UInt64 HugeTLBfs::getFreeHugePages(HugePageSize size) const
{
    if (size == HugePageSize::SIZE_2MB) {
        return m_free_hugepages_2mb;
    } else {
        return m_free_hugepages_1gb;
    }
}

UInt64 HugeTLBfs::getTotalHugePages(HugePageSize size) const
{
    if (size == HugePageSize::SIZE_2MB) {
        return m_nr_hugepages_2mb;
    } else {
        return m_nr_hugepages_1gb;
    }
}

UInt64 HugeTLBfs::getAppHugePages(int app_id, HugePageSize size) const
{
    if (size == HugePageSize::SIZE_2MB) {
        auto it = m_app_usage_2mb.find(app_id);
        return (it != m_app_usage_2mb.end()) ? it->second : 0;
    } else {
        auto it = m_app_usage_1gb.find(app_id);
        return (it != m_app_usage_1gb.end()) ? it->second : 0;
    }
}

// ============================================================================
// Private Helpers
// ============================================================================

int HugeTLBfs::findFreeHugePage(HugePageSize size)
{
    std::vector<HugePageInfo>& pool = (size == HugePageSize::SIZE_2MB) ? m_pool_2mb : m_pool_1gb;

    for (size_t i = 0; i < pool.size(); i++) {
        if (!pool[i].in_use) {
            return static_cast<int>(i);
        }
    }

    return -1;  // No free page found
}

// ============================================================================
// Debug / Logging
// ============================================================================

void HugeTLBfs::dumpPoolState()
{
    m_log_file << "========================================" << std::endl;
    m_log_file << "[HUGETLBFS] Pool State Dump" << std::endl;
    m_log_file << "========================================" << std::endl;
    m_log_file << "2MB Pool: " << m_free_hugepages_2mb << "/" << m_nr_hugepages_2mb << " free" << std::endl;
    m_log_file << "1GB Pool: " << m_free_hugepages_1gb << "/" << m_nr_hugepages_1gb << " free" << std::endl;
    m_log_file << std::endl;

    m_log_file << "Statistics:" << std::endl;
    m_log_file << "  2MB allocations:   " << m_stats.allocations_2mb << std::endl;
    m_log_file << "  1GB allocations:   " << m_stats.allocations_1gb << std::endl;
    m_log_file << "  2MB deallocations: " << m_stats.deallocations_2mb << std::endl;
    m_log_file << "  1GB deallocations: " << m_stats.deallocations_1gb << std::endl;
    m_log_file << "  Allocation failures: " << m_stats.allocation_failures << std::endl;
    m_log_file << "  Pool exhausted events: " << m_stats.pool_exhausted_events << std::endl;
    m_log_file << std::endl;

    m_log_file << "Per-App Usage (2MB):" << std::endl;
    for (const auto& kv : m_app_usage_2mb) {
        m_log_file << "  App " << kv.first << ": " << kv.second << " pages" << std::endl;
    }

    m_log_file << "Per-App Usage (1GB):" << std::endl;
    for (const auto& kv : m_app_usage_1gb) {
        m_log_file << "  App " << kv.first << ": " << kv.second << " pages" << std::endl;
    }
    m_log_file << "========================================" << std::endl;
}
