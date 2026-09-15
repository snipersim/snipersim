#pragma once
/**
 * @file kswapd.h
 * @brief kswapd - Page Reclamation Daemon
 *
 * Simulates Linux's kswapd, which handles memory pressure by reclaiming
 * pages from an LRU-based inactive list when free memory drops below
 * configurable watermark thresholds.
 *
 * LRU Model:
 *   - Active list: recently accessed pages (hot)
 *   - Inactive list: pages not accessed recently (candidates for eviction)
 *   - Pages start on the inactive list and get promoted to active on re-access
 *   - kswapd moves cold pages from active to inactive, and evicts from inactive
 *
 * Watermark Model (simplified Linux zones):
 *   - pages_high: kswapd stops reclaiming
 *   - pages_low:  kswapd starts background reclaiming
 *   - pages_min:  direct reclaim (synchronous, blocks the allocating thread)
 *
 * NO simulator.h, config.hpp or stats.h included here.
 */

#include "debug_config.h"
#include "fixed_types.h"
#include <pthread.h>
#include <atomic>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <unistd.h>
#include <functional>

struct KswapdConfig {
    bool     enabled               = false;
    UInt64   scan_interval_us      = 10000;    // Check interval (simulated time for HOOK_PERIODIC)
    UInt64   pages_high            = 8192;     // Stop reclaiming above this many free pages
    UInt64   pages_low             = 4096;     // Start background reclaiming below this
    UInt64   pages_min             = 1024;     // Direct reclaim threshold (OOM danger)
    UInt64   batch_size            = 32;       // Pages to reclaim per scan
    UInt64   active_to_inactive    = 64;       // Pages to demote from active to inactive per scan
};

template <typename Policy>
class Kswapd : private Policy {
public:
    struct Stats {
        UInt64 scans                 = 0;
        UInt64 pages_reclaimed       = 0;    // Pages evicted (swapped out or freed)
        UInt64 pages_demoted         = 0;    // Pages moved from active to inactive
        UInt64 pages_promoted        = 0;    // Pages moved from inactive to active (on re-access)
        UInt64 direct_reclaims       = 0;    // Synchronous reclaims (below pages_min)
        UInt64 oom_events            = 0;    // Times we couldn't reclaim enough
        UInt64 active_list_size      = 0;    // Current active list size (snapshot)
        UInt64 inactive_list_size    = 0;    // Current inactive list size (snapshot)
    } m_stats;

    Stats& getStats() { return m_stats; }

    explicit Kswapd(const KswapdConfig& config)
        : m_config(config)
        , m_running(false)
        , m_thread_id(0)
        , m_free_pages_ptr(nullptr)
        , m_total_pages(0)
        , m_evict_fn(nullptr)
    {
        Policy::on_init(this);
    }

    ~Kswapd() { stop(); }

    /**
     * @brief Bind to allocator's free page counter and eviction function.
     *
     * @param free_pages_ptr  Pointer to the allocator's free page count
     * @param total_pages     Total physical pages in the system
     * @param evict_fn        Function to call when evicting a page (e.g., swap out)
     */
    void bind(UInt64* free_pages_ptr, UInt64 total_pages,
              std::function<bool(UInt64 vpn)> evict_fn = nullptr)
    {
        m_free_pages_ptr = free_pages_ptr;
        m_total_pages = total_pages;
        m_evict_fn = evict_fn;
    }

    void start()
    {
        if (!m_config.enabled || m_running.load()) return;
        m_running.store(true);
        pthread_create(&m_thread_id, nullptr, &Kswapd::thread_entry, this);
    }

    void stop()
    {
        if (!m_running.load()) return;
        m_running.store(false);
        if (m_thread_id != 0) {
            pthread_join(m_thread_id, nullptr);
            m_thread_id = 0;
        }
    }

    bool isRunning() const { return m_running.load(); }

    // ========================================================================
    // Page access tracking (called by the MMU on every translation)
    // ========================================================================

    /**
     * @brief Notify kswapd that a page was accessed.
     *
     * If the page is on the inactive list, promote it to active.
     * If not tracked yet, add to inactive list.
     */
    void page_accessed(UInt64 vpn)
    {
        auto it = m_page_location.find(vpn);
        if (it == m_page_location.end()) {
            // New page: add to inactive list
            m_inactive_list.push_front(vpn);
            m_page_location[vpn] = PageLocation::INACTIVE;
        } else if (it->second == PageLocation::INACTIVE) {
            // Promote from inactive to active
            m_inactive_list.remove(vpn);
            m_active_list.push_front(vpn);
            it->second = PageLocation::ACTIVE;
            m_stats.pages_promoted++;
        }
        // If already active, do nothing (could update position for true LRU, but
        // Linux uses a simpler two-list model)
    }

    // ========================================================================
    // Reclamation (called periodically or on demand)
    // ========================================================================

    /**
     * @brief Perform a single reclamation scan.
     * @return Number of pages reclaimed.
     */
    UInt64 scan_once()
    {
        m_stats.scans++;
        UInt64 free_pages = m_free_pages_ptr ? *m_free_pages_ptr : m_total_pages;
        UInt64 reclaimed = 0;

        // Update snapshot stats
        m_stats.active_list_size = m_active_list.size();
        m_stats.inactive_list_size = m_inactive_list.size();

        // Only reclaim if below low watermark
        if (free_pages >= m_config.pages_low)
            return 0;

        // Demote cold pages from active to inactive
        UInt64 to_demote = std::min(m_config.active_to_inactive, (UInt64)m_active_list.size());
        for (UInt64 i = 0; i < to_demote; i++) {
            if (m_active_list.empty()) break;
            UInt64 vpn = m_active_list.back();
            m_active_list.pop_back();
            m_inactive_list.push_back(vpn);
            m_page_location[vpn] = PageLocation::INACTIVE;
            m_stats.pages_demoted++;
        }

        // Evict pages from inactive list tail (coldest pages)
        UInt64 target = m_config.batch_size;
        if (free_pages < m_config.pages_min) {
            // Direct reclaim: be more aggressive
            target = m_config.batch_size * 4;
            m_stats.direct_reclaims++;
        }

        while (reclaimed < target && !m_inactive_list.empty()) {
            UInt64 vpn = m_inactive_list.back();
            m_inactive_list.pop_back();

            // Attempt eviction (swap out or free)
            bool evicted = true;
            if (m_evict_fn) {
                evicted = m_evict_fn(vpn);
            }

            if (evicted) {
                m_page_location.erase(vpn);
                reclaimed++;
                m_stats.pages_reclaimed++;
            } else {
                // Can't evict: put back on inactive (e.g., page is pinned)
                m_inactive_list.push_front(vpn);
            }
        }

        if (reclaimed == 0 && free_pages < m_config.pages_min) {
            m_stats.oom_events++;
#if DEBUG_KSWAPD >= DEBUG_BASIC
            Policy::log("[kswapd] OOM: cannot reclaim pages, free=" + std::to_string(free_pages));
#endif
        }

#if DEBUG_KSWAPD >= DEBUG_BASIC
        if (reclaimed > 0) {
            Policy::log("[kswapd] scan #" + std::to_string(m_stats.scans) +
                         ": reclaimed=" + std::to_string(reclaimed) +
                         " free=" + std::to_string(free_pages) +
                         " active=" + std::to_string(m_active_list.size()) +
                         " inactive=" + std::to_string(m_inactive_list.size()));
        }
#endif

        return reclaimed;
    }

private:
    enum class PageLocation { ACTIVE, INACTIVE };

    KswapdConfig m_config;
    std::atomic<bool> m_running;
    pthread_t m_thread_id;

    // Allocator binding
    UInt64* m_free_pages_ptr;
    UInt64 m_total_pages;
    std::function<bool(UInt64)> m_evict_fn;

    // LRU lists
    std::list<UInt64> m_active_list;     // Front = most recent, back = coldest
    std::list<UInt64> m_inactive_list;   // Front = most recent, back = coldest
    std::unordered_map<UInt64, PageLocation> m_page_location;

    static void* thread_entry(void* arg)
    {
        auto* self = static_cast<Kswapd*>(arg);
        self->run_loop();
        return nullptr;
    }

    void run_loop()
    {
        while (m_running.load()) {
            usleep(m_config.scan_interval_us);
            if (!m_running.load()) break;
            scan_once();
        }
    }
};
