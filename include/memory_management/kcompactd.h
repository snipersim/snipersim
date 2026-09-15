#pragma once
/**
 * @file kcompactd.h
 * @brief kcompactd - Memory Compaction Daemon
 *
 * Simulates Linux's kcompactd, which periodically scans physical memory
 * for fragmentation and migrates 4KB pages to create contiguous 2MB-aligned
 * regions suitable for THP promotion.
 *
 * Design:
 *   - Runs on a POSIX thread (Sniper captures it as a simulated thread)
 *   - Periodically wakes up and scans the allocator's 2MB region map
 *   - Identifies "source" regions (low utilization) and "target" regions (high utilization)
 *   - Migrates pages from source to target to consolidate
 *   - Frees emptied source regions back to the buddy allocator
 *   - Models migration latency (page copy + TLB shootdown)
 *
 * NO simulator.h, config.hpp or stats.h included here.
 * Configuration is passed via CompactdConfig struct.
 * Statistics are exposed via getStats() for policy-based registration.
 */

#include "debug_config.h"
#include "fixed_types.h"
#include <pthread.h>
#include <atomic>
#include <vector>
#include <algorithm>
#include <bitset>
#include <map>
#include <tuple>
#include <iostream>
#include <functional>   // std::function (not pulled in transitively on GCC 13 / Ubuntu 24.04)
#include <unistd.h>

struct CompactdConfig {
    bool     enabled              = false;
    UInt64   scan_interval_us     = 10000;    // Microseconds between scans (wall-clock)
    double   low_util_threshold   = 0.25;     // Regions below this utilization are "source" candidates
    double   high_util_threshold  = 0.75;     // Regions above this are "target" candidates
    UInt64   max_pages_per_scan   = 512;      // Max 4KB pages to migrate per scan cycle
    UInt64   migration_latency_ns = 4000;     // Simulated latency per page migration (ns)
    UInt64   shootdown_latency_ns = 5000;     // Simulated TLB shootdown latency per region (ns)
};

template <typename Policy>
class Kcompactd : private Policy {
public:
    struct Stats {
        UInt64 scans              = 0;    // Number of compaction scans performed
        UInt64 pages_migrated     = 0;    // Total 4KB pages migrated
        UInt64 regions_freed      = 0;    // 2MB regions fully freed by compaction
        UInt64 regions_scanned    = 0;    // Total regions examined across all scans
        UInt64 failed_migrations  = 0;    // Migrations that couldn't find a target
        UInt64 total_latency_ns   = 0;    // Cumulative simulated compaction latency
    } m_stats;

    Stats& getStats() { return m_stats; }

    /**
     * @brief Construct the compaction daemon.
     * @param config  Compaction parameters
     *
     * The daemon does NOT start automatically. Call start() to spawn the thread.
     */
    explicit Kcompactd(const CompactdConfig& config)
        : m_config(config)
        , m_running(false)
        , m_thread_id(0)
        , m_two_mb_map(nullptr)
        , m_buddy(nullptr)
    {
        Policy::on_init(this);
    }

    ~Kcompactd()
    {
        stop();
    }

    /**
     * @brief Bind the daemon to an allocator's data structures.
     *
     * Must be called before start(). The pointers must remain valid
     * for the lifetime of the daemon.
     *
     * @param two_mb_map  Pointer to the allocator's 2MB region map
     * @param buddy       Pointer to the buddy allocator (for freeing regions)
     * @param alloc_mutex Pointer to a mutex protecting the allocator (optional, can be nullptr)
     */
    template <typename BuddyType>
    void bind(std::map<UInt64, std::tuple<UInt64, std::bitset<512>, bool>>* two_mb_map,
              BuddyType* buddy,
              pthread_mutex_t* alloc_mutex = nullptr)
    {
        m_two_mb_map = two_mb_map;
        // Store buddy as void* since we're template-erasing it
        m_buddy = static_cast<void*>(buddy);
        m_alloc_mutex = alloc_mutex;
        m_free_fn = [](void* b, UInt64 start, UInt64 end) {
            static_cast<BuddyType*>(b)->free(start, end);
        };
    }

    /**
     * @brief Start the compaction daemon on a new POSIX thread.
     */
    void start()
    {
        if (!m_config.enabled || m_running.load())
            return;

        if (!m_two_mb_map || !m_buddy) {
            std::cerr << "[kcompactd] Cannot start: not bound to allocator" << std::endl;
            return;
        }

        m_running.store(true);
        int rc = pthread_create(&m_thread_id, nullptr, &Kcompactd::thread_entry, this);
        if (rc != 0) {
            std::cerr << "[kcompactd] Failed to create thread: " << rc << std::endl;
            m_running.store(false);
        }
    }

    /**
     * @brief Stop the daemon and join the thread.
     */
    void stop()
    {
        if (!m_running.load())
            return;
        m_running.store(false);
        if (m_thread_id != 0) {
            pthread_join(m_thread_id, nullptr);
            m_thread_id = 0;
        }
    }

    bool isRunning() const { return m_running.load(); }

    /**
     * @brief Perform a single compaction scan (can be called manually for testing).
     * @return Number of pages migrated in this scan.
     */
    UInt64 compact_once()
    {
        if (!m_two_mb_map || !m_buddy)
            return 0;

        // Lock the allocator if a mutex was provided
        if (m_alloc_mutex)
            pthread_mutex_lock(m_alloc_mutex);

        m_stats.scans++;
        UInt64 pages_migrated_this_scan = 0;

        // Classify regions into sources (low util) and targets (high util, not promoted)
        std::vector<UInt64> sources;  // Region indices with low utilization
        std::vector<UInt64> targets;  // Region indices with high utilization (room for more)

        for (auto& kv : *m_two_mb_map) {
            m_stats.regions_scanned++;
            auto& bitset = std::get<1>(kv.second);
            bool promoted = std::get<2>(kv.second);
            double util = static_cast<double>(bitset.count()) / 512.0;

            if (promoted)
                continue;  // Skip fully promoted regions

            if (util > 0 && util <= m_config.low_util_threshold)
                sources.push_back(kv.first);
            else if (util >= m_config.high_util_threshold && util < 1.0)
                targets.push_back(kv.first);
        }

        // Sort sources by ascending utilization (most empty first)
        std::sort(sources.begin(), sources.end(), [this](UInt64 a, UInt64 b) {
            return std::get<1>((*m_two_mb_map)[a]).count() < std::get<1>((*m_two_mb_map)[b]).count();
        });

#if DEBUG_COMPACTD >= DEBUG_BASIC
        Policy::log("[kcompactd] scan #" + std::to_string(m_stats.scans) +
                     ": sources=" + std::to_string(sources.size()) +
                     " targets=" + std::to_string(targets.size()));
#endif

        // Migrate pages from source regions to target regions
        size_t target_idx = 0;
        for (UInt64 src_region : sources) {
            if (pages_migrated_this_scan >= m_config.max_pages_per_scan)
                break;

            auto& src_entry = (*m_two_mb_map)[src_region];
            auto& src_bitset = std::get<1>(src_entry);
            UInt64 src_base = std::get<0>(src_entry);

            // For each allocated page in the source region, try to migrate it
            // to a target region that has a free slot
            for (int bit = 0; bit < 512 && pages_migrated_this_scan < m_config.max_pages_per_scan; bit++) {
                if (!src_bitset.test(bit))
                    continue;  // Page not allocated, skip

                // Find a target with a free slot
                bool migrated = false;
                while (target_idx < targets.size()) {
                    UInt64 tgt_region = targets[target_idx];
                    // Don't migrate to ourselves
                    if (tgt_region == src_region) {
                        target_idx++;
                        continue;
                    }

                    auto& tgt_entry = (*m_two_mb_map)[tgt_region];
                    auto& tgt_bitset = std::get<1>(tgt_entry);

                    // Find a free slot in the target
                    for (int tgt_bit = 0; tgt_bit < 512; tgt_bit++) {
                        if (!tgt_bitset.test(tgt_bit)) {
                            // Migrate: mark target as used, source as free
                            tgt_bitset.set(tgt_bit);
                            src_bitset.reset(bit);
                            pages_migrated_this_scan++;
                            m_stats.pages_migrated++;
                            m_stats.total_latency_ns += m_config.migration_latency_ns;
                            migrated = true;
                            break;
                        }
                    }

                    if (migrated)
                        break;

                    // Target is full, move to next
                    if (tgt_bitset.count() >= 512)
                        target_idx++;
                    else
                        break;  // Still has room, try next source page
                }

                if (!migrated) {
                    m_stats.failed_migrations++;
                }
            }

            // Check if source region is now completely empty
            if (src_bitset.count() == 0) {
                // Free the entire 2MB region back to buddy
                UInt64 region_begin = std::get<0>(src_entry);
                for (int j = 0; j < 512; j++) {
                    m_free_fn(m_buddy, region_begin + j, region_begin + j + 1);
                }
                m_two_mb_map->erase(src_region);
                m_stats.regions_freed++;
                m_stats.total_latency_ns += m_config.shootdown_latency_ns;

#if DEBUG_COMPACTD >= DEBUG_BASIC
                Policy::log("[kcompactd] freed region " + std::to_string(src_region));
#endif
            }
        }

        if (m_alloc_mutex)
            pthread_mutex_unlock(m_alloc_mutex);

#if DEBUG_COMPACTD >= DEBUG_BASIC
        Policy::log("[kcompactd] scan complete: migrated=" + std::to_string(pages_migrated_this_scan) +
                     " freed_regions=" + std::to_string(m_stats.regions_freed));
#endif

        return pages_migrated_this_scan;
    }

private:
    CompactdConfig m_config;
    std::atomic<bool> m_running;
    pthread_t m_thread_id;

    // Allocator data (bound via bind())
    std::map<UInt64, std::tuple<UInt64, std::bitset<512>, bool>>* m_two_mb_map;
    void* m_buddy;
    pthread_mutex_t* m_alloc_mutex = nullptr;
    std::function<void(void*, UInt64, UInt64)> m_free_fn;

    /**
     * @brief Thread entry point (static, POSIX-compatible).
     */
    static void* thread_entry(void* arg)
    {
        auto* self = static_cast<Kcompactd*>(arg);
        self->run_loop();
        return nullptr;
    }

    /**
     * @brief Main daemon loop. Sleeps between scans.
     */
    void run_loop()
    {
#if DEBUG_COMPACTD >= DEBUG_BASIC
        Policy::log("[kcompactd] daemon started, interval=" +
                     std::to_string(m_config.scan_interval_us) + "us");
#endif

        while (m_running.load()) {
            // Sleep for the configured interval (usleep, POSIX-compatible with Sniper)
            usleep(m_config.scan_interval_us);

            if (!m_running.load())
                break;

            compact_once();
        }

#if DEBUG_COMPACTD >= DEBUG_BASIC
        Policy::log("[kcompactd] daemon stopped. Total scans=" + std::to_string(m_stats.scans) +
                     " pages_migrated=" + std::to_string(m_stats.pages_migrated) +
                     " regions_freed=" + std::to_string(m_stats.regions_freed));
#endif
    }
};
