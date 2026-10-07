#pragma once
/**
 * @file khugepaged.h
 * @brief khugepaged - THP Coalescing Daemon
 *
 * Simulates Linux's khugepaged, which periodically scans physical memory
 * for 2MB-aligned regions where nearly all 512 4KB pages are allocated,
 * and promotes them to transparent huge pages (THP).
 *
 * In Linux, khugepaged collapses 512 base pages into a 2MB THP when:
 *   - All 512 pages in the 2MB-aligned region are present
 *   - Or utilization exceeds a promotion threshold (e.g., 95%)
 *
 * Design:
 *   - Sniper-space: runs via HOOK_PERIODIC at simulated time intervals
 *   - Userspace MimicOS: runs as a POSIX thread (SDE-instrumented)
 *   - Scans the allocator's two_mb_map for promotion candidates
 *   - Models TLB shootdown latency for promoted regions
 *
 * NO simulator.h, config.hpp or stats.h included here.
 */

#include "debug_config.h"
#include "fixed_types.h"
#include <pthread.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <algorithm>
#include <bitset>
#include <map>
#include <tuple>
#include <iostream>
#include <unistd.h>

struct KhugepagedConfig {
    bool     enabled                = false;
    UInt64   scan_interval_us       = 10000;   // Microseconds between scans (simulated time)
    double   promotion_threshold    = 0.95;    // Fraction of 512 pages needed to promote (e.g., 0.95 = 487/512)
    UInt64   max_promotions_per_scan = 16;     // Max regions to promote per scan
    UInt64   shootdown_latency_ns   = 5000;    // TLB shootdown latency per promotion (ns)
};

template <typename Policy>
class Khugepaged : private Policy {
public:
    struct Stats {
        UInt64 scans                = 0;
        UInt64 regions_scanned      = 0;
        UInt64 regions_promoted     = 0;
        UInt64 pages_collapsed      = 0;    // Total 4KB pages collapsed into 2MB
        UInt64 total_latency_ns     = 0;
    } m_stats;

    Stats& getStats() { return m_stats; }

    explicit Khugepaged(const KhugepagedConfig& config)
        : m_config(config)
        , m_running(false)
        , m_thread_id(0)
        , m_two_mb_map(nullptr)
    {
        Policy::on_init(this);
    }

    ~Khugepaged() { stop(); }

    /**
     * @brief Bind to an allocator's 2MB region map.
     */
    void bind(std::map<UInt64, std::tuple<UInt64, std::bitset<512>, bool>>* two_mb_map)
    {
        m_two_mb_map = two_mb_map;
    }

    /**
     * @brief Start the daemon on a POSIX thread (for userspace MimicOS).
     */
    void start()
    {
        if (!m_config.enabled || m_running.load() || !m_two_mb_map)
            return;
        m_running.store(true);
        pthread_create(&m_thread_id, nullptr, &Khugepaged::thread_entry, this);
    }

    void stop()
    {
        if (!m_running.load()) return;
        {
            std::lock_guard<std::mutex> lock(m_wake_mutex);
            m_running.store(false);
        }
        m_wake_cv.notify_all();
        if (m_thread_id != 0) {
            pthread_join(m_thread_id, nullptr);
            m_thread_id = 0;
        }
    }

    bool isRunning() const { return m_running.load(); }

    /**
     * @brief Perform a single promotion scan.
     * @return Number of regions promoted.
     */
    UInt64 scan_once()
    {
        if (!m_two_mb_map) return 0;

        m_stats.scans++;
        UInt64 promoted_this_scan = 0;

        for (auto& kv : *m_two_mb_map) {
            if (promoted_this_scan >= m_config.max_promotions_per_scan)
                break;

            m_stats.regions_scanned++;
            auto& bitset = std::get<1>(kv.second);
            bool already_promoted = std::get<2>(kv.second);

            if (already_promoted)
                continue;

            double util = static_cast<double>(bitset.count()) / 512.0;

            if (util >= m_config.promotion_threshold) {
                // Promote: set all remaining bits and mark as promoted
                // In a real OS, this would involve copying pages and updating PTEs
                // Here we just flip the promoted flag
                std::get<2>(kv.second) = true;
                promoted_this_scan++;
                m_stats.regions_promoted++;
                m_stats.pages_collapsed += bitset.count();
                m_stats.total_latency_ns += m_config.shootdown_latency_ns;

#if DEBUG_KHUGEPAGED >= DEBUG_BASIC
                Policy::log("[khugepaged] promoted region " + std::to_string(kv.first) +
                             " (util=" + std::to_string(util) + ", pages=" + std::to_string(bitset.count()) + ")");
#endif
            }
        }

#if DEBUG_KHUGEPAGED >= DEBUG_BASIC
        if (promoted_this_scan > 0) {
            Policy::log("[khugepaged] scan #" + std::to_string(m_stats.scans) +
                         ": promoted " + std::to_string(promoted_this_scan) + " regions");
        }
#endif

        return promoted_this_scan;
    }

private:
    KhugepagedConfig m_config;
    std::atomic<bool> m_running;
    pthread_t m_thread_id;
    std::mutex m_wake_mutex;                 // guards m_running for the interval wait
    std::condition_variable m_wake_cv;       // stop() wakes a parked daemon through it
    std::map<UInt64, std::tuple<UInt64, std::bitset<512>, bool>>* m_two_mb_map;

    static void* thread_entry(void* arg)
    {
        auto* self = static_cast<Khugepaged*>(arg);
        self->run_loop();
        return nullptr;
    }

    /* The interval wait, as a kernel thread does it: parked until the
       interval elapses or stop() wakes it (kthread_stop(), not a sleep to
       be waited out).  A timed condition wait reaches the simulator as a
       futex with a deadline on the emulated clock, so the interval is a
       simulated one and the wake-up at stop() is immediate.  A usleep()
       here could not be interrupted: the kernel's exit then sat out the
       rest of the interval (10ms by default) in simulated time, which
       the run's elapsed time reported as the application's. */
    void wait_interval(std::unique_lock<std::mutex>& lock)
    {
        m_wake_cv.wait_for(lock, std::chrono::microseconds(m_config.scan_interval_us),
                           [this] { return !m_running.load(); });
    }

    void run_loop()
    {
        std::unique_lock<std::mutex> lock(m_wake_mutex);
        while (m_running.load()) {
            wait_interval(lock);
            if (!m_running.load()) break;
            lock.unlock();
            scan_once();
            lock.lock();
        }
    }
};
