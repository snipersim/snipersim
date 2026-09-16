#pragma once

/* Two different classes named HugeTLBfs exist in this tree, and they are not
   interchangeable:

     common/system/memory_management/hugetlbfs/hugetlbfs.h
         the concrete class the simulator uses, guarded by HUGETLBFS_H;
     this file
         a simulator-agnostic HugeTLBfs<Policy>, used by the userspace MimicOS
         kernel, which cannot link against the simulator's own.

   Nothing includes both today, and nothing should: they would collide on the
   class name rather than be skipped, since the guards differ.  Saying so here
   turns a confusing redefinition error into a clear one -- the tree has
   already been bitten once by two same-named classes behind mismatched
   guards, in the legacy TLB removed earlier on this branch. */
#if defined(HUGETLBFS_H)
#  error "Both HugeTLBfs headers are in this translation unit. \
Include either the simulator's hugetlbfs/hugetlbfs.h or this templated one, not both."
#endif

/*------------------------------------------------------------------------------
 *  HugeTLBfs<Policy> - Huge Page Filesystem OS Service (simulator-agnostic)
 *
 *  Policy-based: only Policy::on_init(this) differs between Sniper and Virtuoso.
 *  The Policy opens log_stream and optionally registers stats metrics.
 *
 *  NO simulator.h, config.hpp or stats.h included here.
 *----------------------------------------------------------------------------*/
#include "debug_config.h"
#include "fixed_types.h"

#include <string>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <cassert>

// Huge page size constants (in 4KB base pages)
#define HUGEPAGE_2MB_PAGES  512      // 2MB = 512 * 4KB
#define HUGEPAGE_1GB_PAGES  262144   // 1GB = 262144 * 4KB

// Huge page size in bits (for page_size field)
#define HUGEPAGE_2MB_BITS   21       // 2^21 = 2MB
#define HUGEPAGE_1GB_BITS   30       // 2^30 = 1GB

/**
 * @brief Configuration struct passed to HugeTLBfs constructor.
 *
 * Decouples construction from any simulator config system.
 */
struct HugeTLBfsConfig {
    bool    enabled;
    UInt64  nr_hugepages_2mb;
    UInt64  nr_hugepages_1gb;
    bool    overcommit;
    IntPtr  pool_base_ppn;       // base physical page number for the pool
};

/**
 * @brief HugeTLBfs<Policy> - Huge Page Filesystem Service
 *
 * Manages a pool of pre-allocated huge pages that applications can
 * request for memory-mapped regions requiring large page support.
 */
template <typename Policy>
class HugeTLBfs : private Policy {
public:
    // ========================================================================
    // Types
    // ========================================================================

    /**
     * @brief Huge page size enumeration
     */
    enum class HugePageSize {
        SIZE_2MB = 21,   // 2MB huge pages
        SIZE_1GB = 30    // 1GB huge pages
    };

    /**
     * @brief Information about a single huge page allocation
     */
    struct HugePageInfo {
        IntPtr       base_ppn;   // Base physical page number (in 4KB pages)
        int          app_id;     // Application that owns this page
        IntPtr       vaddr;      // Virtual address mapped to this huge page
        HugePageSize size;       // Size of this huge page
        bool         in_use;     // Whether this page is currently allocated
    };

    /**
     * @brief Statistics for hugetlbfs operations
     */
    struct Stats {
        UInt64 allocations_2mb;         // Successful 2MB allocations
        UInt64 allocations_1gb;         // Successful 1GB allocations
        UInt64 deallocations_2mb;       // 2MB page deallocations
        UInt64 deallocations_1gb;       // 1GB page deallocations
        UInt64 allocation_failures;     // Failed allocation attempts (pool exhausted)
        UInt64 pool_exhausted_events;   // Times the pool was completely empty
    };

    // ========================================================================
    // Public members (accessible by Policy via on_init)
    // ========================================================================

    std::ofstream log_stream;

    Stats&       getStats()       { return m_stats; }
    const Stats& getStats() const { return m_stats; }

    // ========================================================================
    // Construction / Destruction
    // ========================================================================

    /**
     * @brief Construct HugeTLBfs service from a Config struct.
     *
     * No simulator calls here -- the Policy handles log-file opening
     * and stats registration in on_init().
     */
    explicit HugeTLBfs(const HugeTLBfsConfig& cfg)
        : m_enabled(cfg.enabled)
        , m_nr_hugepages_2mb(cfg.nr_hugepages_2mb)
        , m_free_hugepages_2mb(0)
        , m_nr_hugepages_1gb(cfg.nr_hugepages_1gb)
        , m_free_hugepages_1gb(0)
        , m_overcommit(cfg.overcommit)
        , m_pool_base_ppn(cfg.pool_base_ppn)
    {
        if (!m_enabled) {
            std::cout << "[HUGETLBFS] Service disabled in configuration" << std::endl;
            memset(&m_stats, 0, sizeof(m_stats));
            Policy::on_init(this);
            return;
        }

        std::cout << "[HUGETLBFS] Initializing HugeTLBfs service" << std::endl;
        std::cout << "[HUGETLBFS]   2MB huge pages: " << m_nr_hugepages_2mb << std::endl;
        std::cout << "[HUGETLBFS]   1GB huge pages: " << m_nr_hugepages_1gb << std::endl;
        std::cout << "[HUGETLBFS]   Overcommit: " << (m_overcommit ? "enabled" : "disabled") << std::endl;

        // Initialize statistics
        memset(&m_stats, 0, sizeof(m_stats));

        // Let the policy open log_stream and register stats
        Policy::on_init(this);

        // Initialize the huge page pool
        initializePool();

        log_stream << "[HUGETLBFS] Service initialized successfully" << std::endl;
    }

    ~HugeTLBfs()
    {
        if (log_stream.is_open()) {
            dumpPoolState();
            log_stream.close();
        }
        std::cout << "[HUGETLBFS] Service destroyed" << std::endl;
    }

    // ========================================================================
    // Huge Page Allocation Interface
    // ========================================================================

    IntPtr allocateHugePage(int app_id, IntPtr vaddr, HugePageSize size = HugePageSize::SIZE_2MB)
    {
        if (!m_enabled) {
            return static_cast<IntPtr>(-1);
        }

#if DEBUG_HUGETLBFS >= DEBUG_BASIC
        log_stream << "[HUGETLBFS] Allocate request: app=" << app_id
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

#if DEBUG_HUGETLBFS >= DEBUG_BASIC
            log_stream << "[HUGETLBFS] Allocation FAILED: pool exhausted" << std::endl;
#endif
            return static_cast<IntPtr>(-1);
        }

        // Allocate the page
        std::vector<HugePageInfo>& pool = (size == HugePageSize::SIZE_2MB) ? m_pool_2mb : m_pool_1gb;
        HugePageInfo& page = pool[idx];

        page.in_use = true;
        page.app_id = app_id;
        page.vaddr  = vaddr;

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

#if DEBUG_HUGETLBFS >= DEBUG_BASIC
        log_stream << "[HUGETLBFS] Allocated: base_ppn=0x" << std::hex << page.base_ppn << std::dec
                   << " to app=" << app_id << std::endl;
#endif

        return page.base_ppn;
    }

    bool deallocateHugePage(IntPtr base_ppn, HugePageSize size = HugePageSize::SIZE_2MB)
    {
        if (!m_enabled) {
            return false;
        }

#if DEBUG_HUGETLBFS >= DEBUG_BASIC
        log_stream << "[HUGETLBFS] Deallocate request: base_ppn=0x" << std::hex << base_ppn << std::dec
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
                pool[i].vaddr  = 0;

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

#if DEBUG_HUGETLBFS >= DEBUG_BASIC
                log_stream << "[HUGETLBFS] Deallocated: base_ppn=0x" << std::hex << base_ppn << std::dec
                           << " from app=" << app_id << std::endl;
#endif
                return true;
            }
        }

#if DEBUG_HUGETLBFS >= DEBUG_BASIC
        log_stream << "[HUGETLBFS] Deallocate FAILED: page not found" << std::endl;
#endif
        return false;
    }

    int deallocateAllForApp(int app_id)
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
                page.vaddr  = 0;
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
                page.vaddr  = 0;
                m_free_hugepages_1gb++;
                m_stats.deallocations_1gb++;
                count++;
            }
        }

        // Clear app usage tracking
        m_app_usage_2mb.erase(app_id);
        m_app_usage_1gb.erase(app_id);

#if DEBUG_HUGETLBFS >= DEBUG_BASIC
        log_stream << "[HUGETLBFS] Deallocated all for app=" << app_id
                   << " count=" << count << std::endl;
#endif

        return count;
    }

    // ========================================================================
    // Pool Query Interface
    // ========================================================================

    UInt64 getFreeHugePages(HugePageSize size = HugePageSize::SIZE_2MB) const
    {
        if (size == HugePageSize::SIZE_2MB) {
            return m_free_hugepages_2mb;
        } else {
            return m_free_hugepages_1gb;
        }
    }

    UInt64 getTotalHugePages(HugePageSize size = HugePageSize::SIZE_2MB) const
    {
        if (size == HugePageSize::SIZE_2MB) {
            return m_nr_hugepages_2mb;
        } else {
            return m_nr_hugepages_1gb;
        }
    }

    UInt64 getAppHugePages(int app_id, HugePageSize size = HugePageSize::SIZE_2MB) const
    {
        if (size == HugePageSize::SIZE_2MB) {
            auto it = m_app_usage_2mb.find(app_id);
            return (it != m_app_usage_2mb.end()) ? it->second : 0;
        } else {
            auto it = m_app_usage_1gb.find(app_id);
            return (it != m_app_usage_1gb.end()) ? it->second : 0;
        }
    }

    bool isEnabled() const { return m_enabled; }

    // ========================================================================
    // Debug / Logging
    // ========================================================================

    void dumpPoolState()
    {
        log_stream << "========================================" << std::endl;
        log_stream << "[HUGETLBFS] Pool State Dump" << std::endl;
        log_stream << "========================================" << std::endl;
        log_stream << "2MB Pool: " << m_free_hugepages_2mb << "/" << m_nr_hugepages_2mb << " free" << std::endl;
        log_stream << "1GB Pool: " << m_free_hugepages_1gb << "/" << m_nr_hugepages_1gb << " free" << std::endl;
        log_stream << std::endl;

        log_stream << "Statistics:" << std::endl;
        log_stream << "  2MB allocations:   " << m_stats.allocations_2mb << std::endl;
        log_stream << "  1GB allocations:   " << m_stats.allocations_1gb << std::endl;
        log_stream << "  2MB deallocations: " << m_stats.deallocations_2mb << std::endl;
        log_stream << "  1GB deallocations: " << m_stats.deallocations_1gb << std::endl;
        log_stream << "  Allocation failures: " << m_stats.allocation_failures << std::endl;
        log_stream << "  Pool exhausted events: " << m_stats.pool_exhausted_events << std::endl;
        log_stream << std::endl;

        log_stream << "Per-App Usage (2MB):" << std::endl;
        for (const auto& kv : m_app_usage_2mb) {
            log_stream << "  App " << kv.first << ": " << kv.second << " pages" << std::endl;
        }

        log_stream << "Per-App Usage (1GB):" << std::endl;
        for (const auto& kv : m_app_usage_1gb) {
            log_stream << "  App " << kv.first << ": " << kv.second << " pages" << std::endl;
        }
        log_stream << "========================================" << std::endl;
    }

private:
    // ========================================================================
    // Private Members
    // ========================================================================

    bool m_enabled;

    // Pool of 2MB huge pages
    std::vector<HugePageInfo> m_pool_2mb;
    UInt64 m_nr_hugepages_2mb;
    UInt64 m_free_hugepages_2mb;

    // Pool of 1GB huge pages
    std::vector<HugePageInfo> m_pool_1gb;
    UInt64 m_nr_hugepages_1gb;
    UInt64 m_free_hugepages_1gb;

    bool m_overcommit;

    // Per-application tracking: app_id -> count of huge pages
    std::unordered_map<int, UInt64> m_app_usage_2mb;
    std::unordered_map<int, UInt64> m_app_usage_1gb;

    // Statistics
    Stats m_stats;

    // Base physical address for huge page pool (in 4KB pages)
    IntPtr m_pool_base_ppn;

    // ========================================================================
    // Private Helpers
    // ========================================================================

    void initializePool()
    {
        IntPtr current_ppn = m_pool_base_ppn;

        // Initialize 2MB huge page pool
        m_pool_2mb.reserve(m_nr_hugepages_2mb);
        for (UInt64 i = 0; i < m_nr_hugepages_2mb; i++) {
            HugePageInfo info;
            info.base_ppn = current_ppn;
            info.app_id   = -1;
            info.vaddr    = 0;
            info.size     = HugePageSize::SIZE_2MB;
            info.in_use   = false;
            m_pool_2mb.push_back(info);
            current_ppn += HUGEPAGE_2MB_PAGES;  // Advance by 512 pages (2MB)
        }
        m_free_hugepages_2mb = m_nr_hugepages_2mb;

        // Initialize 1GB huge page pool
        m_pool_1gb.reserve(m_nr_hugepages_1gb);
        for (UInt64 i = 0; i < m_nr_hugepages_1gb; i++) {
            HugePageInfo info;
            info.base_ppn = current_ppn;
            info.app_id   = -1;
            info.vaddr    = 0;
            info.size     = HugePageSize::SIZE_1GB;
            info.in_use   = false;
            m_pool_1gb.push_back(info);
            current_ppn += HUGEPAGE_1GB_PAGES;  // Advance by 262144 pages (1GB)
        }
        m_free_hugepages_1gb = m_nr_hugepages_1gb;

        log_stream << "[HUGETLBFS] Pool initialized:" << std::endl;
        log_stream << "[HUGETLBFS]   2MB pool: " << m_nr_hugepages_2mb << " pages, base PPN: 0x"
                   << std::hex << m_pool_base_ppn << std::dec << std::endl;
        log_stream << "[HUGETLBFS]   1GB pool: " << m_nr_hugepages_1gb << " pages" << std::endl;
    }

    int findFreeHugePage(HugePageSize size)
    {
        std::vector<HugePageInfo>& pool = (size == HugePageSize::SIZE_2MB) ? m_pool_2mb : m_pool_1gb;

        for (size_t i = 0; i < pool.size(); i++) {
            if (!pool[i].in_use) {
                return static_cast<int>(i);
            }
        }

        return -1;  // No free page found
    }
};
