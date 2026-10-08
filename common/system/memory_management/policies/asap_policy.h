#pragma once
/*
 * Sniper policy of the ASAP allocator [Margaritov et al., MICRO 2019].
 *
 * ASAP's allocator is the reservation-based THP allocator: a 2MB region is
 * reserved on the first touch of a page in it and promoted to a 2MB page
 * once the promotion threshold of its pages is touched, with 4KB pages from
 * the buddy when no region can be reserved.  Upstream carries it as a copy
 * of reserve_thp.h under another name (asap.h); here it is
 * ReservationTHPAllocator instantiated with this policy, which registers
 * the same counters under the allocator's type name (asap.*) and logs
 * through SimLog under DEBUG_RESERVATION_THP.
 */

#include "stats.h"
#include "simulator.h"
#include "config.hpp"
#include "sim_log.h"
#include "debug_config.h"
#include "fixed_types.h"
#include <iostream>

namespace Sniper {
    namespace ASAP {
        struct MetricsPolicy
        {
            mutable SimLog* m_log;

            MetricsPolicy() : m_log(nullptr) {}
            ~MetricsPolicy() { delete m_log; }

            template <typename Allocator>
            void on_init(const String& name, int memory_size, int kernel_size, int threshold_for_promotion, Allocator* phys_mem_alloc)
            {
                auto& stats = phys_mem_alloc->getStats();
                m_log = new SimLog("ASAP", -1, DEBUG_RESERVATION_THP);

                std::cout << "[MimicOS] ASAP Allocator (reservation-based THP): memory " << memory_size << " MB, kernel "
                          << kernel_size << " MB" << std::endl;
                if (m_log->isEnabled())
                    m_log->info("Creating the ASAP allocator: memory", memory_size, "MB kernel", kernel_size, "MB");

                registerStatsMetric(name, 0, "four_kb_allocated", &stats.four_kb_allocated);
                registerStatsMetric(name, 0, "two_mb_reserved", &stats.two_mb_reserved);
                registerStatsMetric(name, 0, "two_mb_promoted", &stats.two_mb_promoted);
                registerStatsMetric(name, 0, "two_mb_demoted", &stats.two_mb_demoted);
                registerStatsMetric(name, 0, "total_allocations", &stats.total_allocations);
                registerStatsMetric(name, 0, "page_table_pages_used", &stats.kernel_pages_used);
            }

            template<typename... Args>
            void log(const Args&... args) const { if (m_log) m_log->debug(args...); }
            template<typename... Args>
            void log_trace(const Args&... args) const { if (m_log) m_log->trace(args...); }
        };
    }
}
