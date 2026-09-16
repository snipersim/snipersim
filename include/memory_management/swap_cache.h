#pragma once

/* As with hugetlbfs.h alongside this file: two different classes named
   SwapCache exist in the tree and are not interchangeable.

     common/system/memory_management/swap/swap_space.h
         the concrete class the simulator uses, behind SWAP_SPACE_H;
     this file
         a simulator-agnostic SwapCache<Policy> for the userspace MimicOS
         kernel, which cannot link against the simulator's own.

   Nothing includes both, and the guards differ, so they would collide on the
   class name rather than one being skipped.  The check makes that explicit. */
#if defined(SWAP_SPACE_H)
#  error "Both SwapCache headers are in this translation unit. \
Include either the simulator's swap/swap_space.h or this templated one, not both."
#endif

/*------------------------------------------------------------------------------
 *  SwapCache<Policy> - Swap Cache (simulator-agnostic)
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
#include <utility>
#include <functional>

/**
 * @brief SwapCache<Policy> - Simulates the Linux swap cache.
 *
 * Manages swap-in / swap-out of pages to a backing store.
 */
template <typename Policy>
class SwapCache : private Policy {
public:
    // ========================================================================
    // Types
    // ========================================================================

    struct Stats {
        UInt64 swap_ins;
        UInt64 swap_outs;
        UInt64 failed_swap_outs_space;
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
     * @brief Construct SwapCache with a given swap size in MB.
     *
     * No simulator calls -- the Policy handles log-file opening
     * and stats registration in on_init().
     *
     * @param swap_size_mb  Swap space size in megabytes
     */
    explicit SwapCache(int swap_size_mb)
        : m_swap_size(swap_size_mb * 1024 / 4)   // convert MB -> number of 4KB pages
    {
        memset(&m_stats, 0, sizeof(m_stats));

        // Initialize free pages
        m_free_pages.resize(m_swap_size, false);  // false = free

        std::cout << "[SWAP_CACHE] Swap cache initialized with size: " << m_swap_size << std::endl;

        // Let the policy open log_stream and register stats
        Policy::on_init(this);
    }

    ~SwapCache() = default;

    // ========================================================================
    // Public Interface
    // ========================================================================

    /**
     * @brief Look up a page in the swap cache.
     *
     * @param page_id  Virtual page identifier
     * @param app_id   Application ID
     * @return true if the page is present in swap
     */
    bool lookup(IntPtr page_id, int app_id)
    {
#if DEBUG_SWAP_CACHE >= DEBUG_BASIC
        log_stream << "[SWAP_CACHE] Looking up page: " << page_id << " for app: " << app_id << std::endl;
#endif
        auto it = m_swap_cache_map.find(std::make_pair(page_id, app_id));
#if DEBUG_SWAP_CACHE >= DEBUG_BASIC
        log_stream << "[SWAP_CACHE] Swap cache size: " << m_swap_cache_map.size() << std::endl;
#endif
        if (it != m_swap_cache_map.end()) {
#if DEBUG_SWAP_CACHE >= DEBUG_BASIC
            log_stream << "[SWAP_CACHE] Page " << page_id << " found in swap cache." << std::endl;
#endif
            return true;
        } else {
#if DEBUG_SWAP_CACHE >= DEBUG_BASIC
            log_stream << "[SWAP_CACHE] Page " << page_id << " not found in swap cache." << std::endl;
#endif
            return false;
        }
    }

    /**
     * @brief Swap out a page to the backing store.
     *
     * @param virtual_page   Virtual page identifier
     * @param app_id         Application ID
     * @param is_memory_full Whether physical memory is full
     * @return true if the page was successfully swapped out
     */
    bool swapOut(IntPtr virtual_page, int app_id, bool is_memory_full)
    {
#if DEBUG_SWAP_CACHE >= DEBUG_BASIC
        log_stream << "[SWAP_CACHE] Attempting to swap out page: " << virtual_page
                   << " for app: " << app_id << std::endl;
#endif

        // Add the page to the swap cache
        IntPtr swap_location = findFreePage();

#if DEBUG_SWAP_CACHE >= DEBUG_BASIC
        log_stream << "[SWAP_CACHE] Swap location found: " << swap_location << std::endl;
#endif

        if (swap_location == static_cast<IntPtr>(-1)) {
            // No free page available in the swap cache
#if DEBUG_SWAP_CACHE >= DEBUG_BASIC
            log_stream << "[SWAP_CACHE] No free page available, cannot swap out page: " << virtual_page << std::endl;
#endif
            return false;
        }

#if DEBUG_SWAP_CACHE >= DEBUG_BASIC
        log_stream << "[SWAP_CACHE] Swapping out page: " << virtual_page
                   << " to swap location: " << swap_location << std::endl;
#endif
        m_swap_cache_map[std::make_pair(virtual_page, app_id)] = swap_location;

        // Get the current size of the swap cache
        int current_size = m_swap_cache_map.size();

#if DEBUG_SWAP_CACHE >= DEBUG_BASIC
        log_stream << "[SWAP_CACHE] Current swap cache size: " << current_size << std::endl;
        log_stream << "[SWAP_CACHE] Swap size limit: " << m_swap_size << std::endl;
#endif

        if (current_size > m_swap_size) {
            m_stats.failed_swap_outs_space++;
#if DEBUG_SWAP_CACHE >= DEBUG_BASIC
            log_stream << "[SWAP_CACHE] Swap cache is full, cannot swap out page: " << virtual_page << std::endl;
#endif
            return false;
        }

        // Successfully swapped out the page
#if DEBUG_SWAP_CACHE >= DEBUG_BASIC
        log_stream << "[SWAP_CACHE] Successfully swapped out page: " << virtual_page
                   << " to swap location: " << swap_location << std::endl;
#endif
        m_stats.swap_outs++;
        return true;
    }

    /**
     * @brief Swap a page back in from the backing store.
     *
     * Removes the page from the swap cache map.
     *
     * @param virtual_page  Virtual page identifier
     * @param app_id        Application ID
     */
    void swapIn(IntPtr virtual_page, int app_id)
    {
        // Remove the page from the swap cache
        m_swap_cache_map.erase(std::make_pair(virtual_page, app_id));

#if DEBUG_SWAP_CACHE >= DEBUG_BASIC
        log_stream << "[SWAP_CACHE] Swapped in page: " << virtual_page << std::endl;
#endif
        m_stats.swap_ins++;
    }

    /**
     * @brief Find a free page slot in the swap backing store.
     *
     * @return Index of the free page, or (IntPtr)-1 if none available
     */
    IntPtr findFreePage()
    {
        for (size_t i = 0; i < m_free_pages.size(); ++i) {
            if (!m_free_pages[i]) {
                m_free_pages[i] = true;  // Mark this page as used
#if DEBUG_SWAP_CACHE >= DEBUG_BASIC
                log_stream << "[SWAP_CACHE] Found free page at index: " << i << std::endl;
#endif
                return i;
            }
        }
#if DEBUG_SWAP_CACHE >= DEBUG_BASIC
        log_stream << "[SWAP_CACHE] No free page found." << std::endl;
#endif
        return static_cast<IntPtr>(-1);
    }

private:
    // ========================================================================
    // Private Members
    // ========================================================================

    int m_swap_size;   // Number of 4KB pages in the swap space

    Stats m_stats;

    // Custom hash function for std::pair<IntPtr, int>
    struct PairHash {
        std::size_t operator()(const std::pair<IntPtr, int>& p) const {
            return std::hash<IntPtr>()(p.first) ^ (std::hash<int>()(p.second) << 1);
        }
    };

    // Custom equality comparator
    struct PairEqual {
        bool operator()(const std::pair<IntPtr, int>& lhs, const std::pair<IntPtr, int>& rhs) const {
            return lhs.first == rhs.first && lhs.second == rhs.second;
        }
    };

    // Swap cache map: (virtual_page, app_id) -> swap_location
    std::unordered_map<std::pair<IntPtr, int>, IntPtr, PairHash, PairEqual> m_swap_cache_map;

    // Bitmap of free pages in the swap backing store
    std::vector<bool> m_free_pages;
};
