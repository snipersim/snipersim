#pragma once
#include <fstream>
#include "simulator.h"
#include "config.hpp"
#include "stats.h"
#include "core_manager.h"
#include "memory_manager.h"
#include "mmu_base.h"
#include "mmu_spec.h"
#include "spec_engine_base.h"
#include "revelator_thp_engine.h"

namespace Sniper {
namespace RevelatorTHP {

struct RevelatorTHPSniperPolicy
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
        registerStatsMetric(name, 0, "revelator_2mb_data_allocated",
                            &gen.revelator_2mb_data_allocated);

        // --- THP stats ---
        auto &thp = alloc.get_thp_stats();
        registerStatsMetric(name, 0, "two_mb_reserved",
                            &thp.two_mb_reserved);
        registerStatsMetric(name, 0, "two_mb_promoted",
                            &thp.two_mb_promoted);
        registerStatsMetric(name, 0, "two_mb_demoted",
                            &thp.two_mb_demoted);
        registerStatsMetric(name, 0, "total_thp_allocations",
                            &thp.total_thp_allocations);
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
                          + ".revelator_thp_allocator.log";
        std::string fullpath =
            std::string(Sim()->getConfig()->getOutputDirectory().c_str())
            + "/" + fname;

        logfile.open(fullpath.c_str());
        alloc->log_stream = &logfile;
    }

    // === 3. Spec-engine feedback on every allocation ===
    // Called at the start of every allocate() call.
    // Reaches into the spec engine to toggle speculation modes
    // based on allocation statistics, mirroring the commented-out
    // code in revelator_thp.cc.
    template<typename GeneralStats, typename THPStats>
    static void on_allocate_feedback(UInt64 core_id,
                                     const GeneralStats &gen,
                                     const THPStats &thp)
    {
        // Compute page counts
        UInt64 total_2mb_pages = gen.revelator_2mb_data_allocated * 512 + thp.two_mb_promoted * 512;
        UInt64 total_4kb_pages = gen.revelator_4kb_data_allocated + gen.data_allocated_fallback;

        UInt64 revelator_allocated = gen.revelator_2mb_data_allocated * 512 + gen.revelator_4kb_data_allocated;
        UInt64 total_allocated = gen.revelator_4kb_data_allocated
                               + gen.revelator_2mb_data_allocated * 512
                               + gen.data_allocated_fallback
                               + thp.two_mb_promoted * 512;

        // Nothing allocated yet — skip
        if (total_allocated == 0)
            return;

        // Get the RevelatorTHP spec engine for this core
        Core* core = Sim()->getCoreManager()->getCoreFromID(core_id);
        if (!core) return;

        ParametricDramDirectoryMSI::MemoryManager* manager_base =
            dynamic_cast<ParametricDramDirectoryMSI::MemoryManager*>(core->getMemoryManager());
        if (!manager_base) return;

        ParametricDramDirectoryMSI::MemoryManagementUnitBase* mmu_base = manager_base->getMMU();
        if (!mmu_base) return;

        ParametricDramDirectoryMSI::MemoryManagementUnitSpec* mmu_spec =
            dynamic_cast<ParametricDramDirectoryMSI::MemoryManagementUnitSpec*>(mmu_base);
        if (!mmu_spec) return;

        ParametricDramDirectoryMSI::SpecEngineBase* spec_engine_base = mmu_spec->getSpecEngine();
        if (!spec_engine_base) return;

        ParametricDramDirectoryMSI::RevelatorTHP* revelator_engine =
            dynamic_cast<ParametricDramDirectoryMSI::RevelatorTHP*>(spec_engine_base);
        if (!revelator_engine) return;

        // Decision logic from the commented-out allocator code:
        // If less than 50% of total pages are placed by Revelator hash, deactivate speculation
        if (revelator_allocated < total_allocated / 2) {
            revelator_engine->deactivateSpeculation();
        }
        // If 2MB pages dominate, switch spec engine to 2MB prediction mode
        else if (total_2mb_pages > total_4kb_pages) {
            revelator_engine->activate2MBmode();
        }
        // Otherwise default to 4KB prediction mode
        else {
            revelator_engine->activate4KBmode();
        }
    }

    // === 4. THP fragmentation target ===
    // Returns the THP-level fragmentation target from config.
    // The allocator calls this during fragment_memory().
    static double get_thp_fragmentation_target(const String &name)
    {
        // Try to read revelator_thp-specific dual fragmentation config.
        // If not found, default to 0.0 (no 2MB poisoning).
        try {
            return 1.0 - Sim()->getCfg()->getFloat("perf_model/" + name + "/target_thp_fragmentation");
        } catch (...) {
            return 0.0;
        }
    }
};

} // namespace RevelatorTHP
} // namespace Sniper
