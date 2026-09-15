#pragma once
#include <fstream>
#include "simulator.h"
#include "config.hpp"
#include "stats.h"

namespace Sniper {
namespace RevelatorSimple {

struct RevelatorSimpleSniperPolicy
{
    static inline std::ofstream logfile;

    // === 1. Stats Registration ===
    template<typename Alloc>
    static void register_stats(const String &name, Alloc &alloc)
    {
        // --- Per-hash stats ---
        auto &hash_stats = alloc.get_hash_stats();
        int num = alloc.get_number_of_hashes();

        for (int i = 0; i < num; i++)
        {
            registerStatsMetric(name, 0,
                "hash" + itostr(i + 1) + "_successful_data_allocations",
                &hash_stats.successful_data_allocations[i]);
            registerStatsMetric(name, 0,
                "hash" + itostr(i + 1) + "_successful_pt_allocations",
                &hash_stats.sucessful_pt_allocations[i]);
        }

        registerStatsMetric(name, 0, "total_hash_data_allocations",
                            &hash_stats.total_hash_data_allocations);
        registerStatsMetric(name, 0, "total_hash_pt_allocations",
                            &hash_stats.total_hash_pt_allocations);

        // --- General stats ---
        auto &gen = alloc.get_general_stats();
        registerStatsMetric(name, 0, "total_page_table_allocations",
                            &gen.total_page_table_allocations);
        registerStatsMetric(name, 0, "total_data_allocations",
                            &gen.total_data_allocations);
        registerStatsMetric(name, 0, "data_allocated_fallback",
                            &gen.data_allocated_fallback);
        registerStatsMetric(name, 0, "pt_allocated_fallback",
                            &gen.pt_allocated_fallback);
        registerStatsMetric(name, 0, "revelator_4kb_data_allocated",
                            &gen.revelator_4kb_data_allocated);
    }

    // === 2. Initialization (Sniper-specific logging) ===
    template<typename Alloc>
    static void on_init(const String &name,
                        UInt64 memory_size,
                        UInt64 kernel_size,
                        int max_order,
                        const String &frag_type,
                        Alloc *alloc)
    {
        std::string fname = std::string(name.c_str())
                          + ".revelator_simple_allocator.log";
        std::string fullpath =
            std::string(Sim()->getConfig()->getOutputDirectory().c_str())
            + "/" + fname;

        logfile.open(fullpath.c_str());
        alloc->log_stream = &logfile;
    }
};

} // namespace RevelatorSimple
} // namespace Sniper
