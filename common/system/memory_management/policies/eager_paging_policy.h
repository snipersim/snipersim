#pragma once
#include "stats.h"
#include "simulator.h"
#include "config.hpp"
#include "sim_log.h"
#include "debug_config.h"
#include <string>

namespace Sniper {
    namespace EagerPaging {
        // The Sniper side of the eager-paging allocator: its statistics
        // and its log (SimLog under DEBUG_EAGER_PAGING).
        struct MetricsPolicy
        {
            mutable SimLog* m_log = nullptr;

            ~MetricsPolicy() { delete m_log; }

            template <typename Alloc>
            void on_init(const String& name, UInt64 memory_size, UInt64 kernel_size,
                         int max_order, const String& frag_type, Alloc* alloc)
            {
                m_log = new SimLog("EagerPaging", -1, DEBUG_EAGER_PAGING);
                std::cout << "[MimicOS] Eager Paging Allocator: " << memory_size << " MB, kernel " << kernel_size
                          << " MB, max order " << max_order << ", fragmentation " << frag_type << std::endl;

                auto& s = alloc->getStats();
                registerStatsMetric(name, 0, "total_allocated_vmas", &s.total_allocated_vmas);
                registerStatsMetric(name, 0, "total_allocated_physical_ranges", &s.total_allocated_physical_ranges);
                registerStatsMetric(name, 0, "total_allocated_pages", &s.total_allocated_pages);
                registerStatsMetric(name, 0, "single_page_allocations", &s.single_page_allocations);
            }

            template <typename Alloc>
            void log(Alloc*, const std::string& msg) const
            {
                if (m_log) m_log->debug(msg);
            }
        };
    }
}
