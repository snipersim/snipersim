#pragma once
/*
 * Sniper policy of the Utopia allocator (utopia.h): configuration reads,
 * statistics registration, logging through SimLog under DEBUG_UTOPIA, and
 * the two OS services the allocator needs -- the application a core runs
 * (RestSeg ownership is per application) and the deletion of a page table
 * entry when a page migrates into a RestSeg.
 */

#include "stats.h"
#include "simulator.h"
#include "config.hpp"
#include "sim_log.h"
#include "debug_config.h"
#include "core_manager.h"
#include "core.h"
#include "thread.h"
#include "mimicos.h"
#include <iostream>
#include <string>

namespace Sniper {
namespace Utopia {

struct MetricsPolicy
{
    mutable SimLog* sim_log;

    MetricsPolicy() : sim_log(nullptr) {}
    ~MetricsPolicy() { delete sim_log; }

    // ---- configuration ----
    // The configuration layer exits on a missing key rather than throwing,
    // so optional keys are read through hasKey (getIntDefault), not try/catch.
    static int read_config_int(const char* key) {
        return Sim()->getCfg()->getInt(key);
    }
    static int read_config_int_default(const char* key, int def) {
        return Sim()->getCfg()->getIntDefault(key, def);
    }
    static int read_config_int_array(const char* key, int index) {
        return Sim()->getCfg()->getIntArray(key, index);
    }
    static int get_num_cores() {
        return Sim()->getConfig()->getApplicationCores();
    }

    // The application whose fault is being served on a core; 0 when the
    // core has no thread (allocations made before any thread runs)
    static int app_id_for_core(UInt64 core_id) {
        if (core_id == (UInt64)-1)
            return 0;
        Core* core = Sim()->getCoreManager()->getCoreFromID((core_id_t)core_id);
        if (core && core->getThread())
            return core->getThread()->getAppId();
        return 0;
    }

    // ---- initialisation and statistics ----
    template <typename AllocatorT>
    void on_init(String name, int memory_size, int kernel_size, AllocatorT* alloc)
    {
        delete sim_log;
        sim_log = new SimLog("UTOPIA", -1, DEBUG_UTOPIA);

        std::cout << "[MimicOS] Utopia Allocator: " << alloc->getNumRestSegs() << " RestSegs, THP promotion threshold "
                  << alloc->getTHPPromotionThreshold() << std::endl;
        for (int i = 0; i < alloc->getNumRestSegs(); i++) {
            auto* rs = alloc->getRestSeg(i);
            std::cout << "[MimicOS]   RestSeg " << i << ": " << rs->getSizeMB() << " MB, page bits " << rs->getPageSizeBits()
                      << ", " << rs->getAssociativity() << "-way, " << rs->getNumSets() << " sets, base ppn " << rs->getBase()
                      << ", radix levels " << rs->getRadixLevels() << std::endl;
        }
        sim_log->info("Init:", name.c_str(), "mem_size=", memory_size, "MB kernel_size=", kernel_size, "MB");

        auto& stats = alloc->getStats();
        registerStatsMetric(name, 0, "restseg_2mb_allocations", &stats.restseg_2mb_allocations);
        registerStatsMetric(name, 0, "restseg_4kb_allocations", &stats.restseg_4kb_allocations);
        registerStatsMetric(name, 0, "flexseg_thp_allocations", &stats.flexseg_thp_allocations);
        registerStatsMetric(name, 0, "flexseg_4kb_allocations", &stats.flexseg_4kb_allocations);
        registerStatsMetric(name, 0, "flexseg_thp_reserved", &stats.flexseg_thp_reserved);
        registerStatsMetric(name, 0, "flexseg_thp_promoted", &stats.flexseg_thp_promoted);
        registerStatsMetric(name, 0, "flexseg_thp_demoted", &stats.flexseg_thp_demoted);
        registerStatsMetric(name, 0, "restseg_allocations", &stats.restseg_allocations);
        registerStatsMetric(name, 0, "flexseg_allocations", &stats.flexseg_allocations);
        registerStatsMetric(name, 0, "restseg_evictions", &stats.restseg_evictions);
        registerStatsMetric(name, 0, "total_allocations", &stats.total_allocations);
        registerStatsMetric(name, 0, "migrations", &stats.migrations);

        // Per RestSeg: its own lookups and placements, the fingerprint shadow
        // statistics, the region-way preference and the radix way table
        for (int i = 0; i < alloc->getNumRestSegs(); i++) {
            auto* rs = alloc->getRestSeg(i);
            String p = "restseg" + itostr(i) + "_";
            registerStatsMetric(name, 0, p + "accesses", rs->getAccessesPtr());
            registerStatsMetric(name, 0, p + "hits", rs->getHitsPtr());
            registerStatsMetric(name, 0, p + "allocations", rs->getAllocationsPtr());
            registerStatsMetric(name, 0, p + "conflicts", rs->getConflictsPtr());
            registerStatsMetric(name, 0, p + "fingerprint_checks", rs->getFingerprintChecksPtr());
            registerStatsMetric(name, 0, p + "fingerprint_matches", rs->getFingerprintMatchesPtr());
            registerStatsMetric(name, 0, p + "fingerprint_false_positives", rs->getFingerprintFalsePositivesPtr());
            registerStatsMetric(name, 0, p + "fingerprint_true_positives", rs->getFingerprintTruePositivesPtr());
            registerStatsMetric(name, 0, p + "region_way_attempts", rs->getRegionWayAttemptsPtr());
            registerStatsMetric(name, 0, p + "region_way_hits", rs->getRegionWayHitsPtr());
            registerStatsMetric(name, 0, p + "region_way_relearns", rs->getRegionWayRelearnsPtr());
            registerStatsMetric(name, 0, p + "global_way_rotations", rs->getGlobalWayRotationsPtr());
            registerStatsMetric(name, 0, p + "radix_lookups", rs->getRadixLookupsPtr());
            registerStatsMetric(name, 0, p + "radix_hits", rs->getRadixHitsPtr());
            registerStatsMetric(name, 0, p + "radix_misses", rs->getRadixMissesPtr());
        }

        if (alloc->isCostTopKEnabled()) {
            auto& st = alloc->getCostTopKState();
            registerStatsMetric(name, 0, "cost_topk_score_updates", &st.score_updates);
            registerStatsMetric(name, 0, "cost_topk_inserts", &st.topk_inserts);
            registerStatsMetric(name, 0, "cost_topk_replacements", &st.topk_replacements);
            registerStatsMetric(name, 0, "cost_topk_migration_attempts", &st.migration_attempts);
            registerStatsMetric(name, 0, "cost_topk_migrations_issued", &st.migrations_issued);
            registerStatsMetric(name, 0, "cost_topk_gated_credits", &st.gated_credits);
            registerStatsMetric(name, 0, "cost_topk_gated_threshold", &st.gated_threshold);
            registerStatsMetric(name, 0, "cost_topk_gated_sticky", &st.gated_sticky);
            registerStatsMetric(name, 0, "cost_topk_gated_cooldown", &st.gated_cooldown);
        }
    }

    // A counter of an allocator derived from the Utopia one (utopia_hash_coalesce.h)
    static void register_counter(const String& name, const String& key, UInt64* ptr) {
        registerStatsMetric(name, 0, key, ptr);
    }

    // ---- logging ----
    template<typename... Args>
    void log(const Args&... args) const { if (sim_log) sim_log->info(args...); }
    template<typename... Args>
    void log_debug(const Args&... args) const { if (sim_log) sim_log->debug(args...); }
    template<typename... Args>
    void log_trace(const Args&... args) const { if (sim_log) sim_log->trace(args...); }

    // ---- OS services ----
    // A page that migrated into a RestSeg is translated by the RestSeg walker;
    // its page table entry would be stale and is removed.
    void deletePageTableEntry(IntPtr address, int app_id) const {
        MimicOS* mimicos = Sim()->getMimicOS();
        if (mimicos)
            mimicos->deletePageTableEntry(address, app_id);
    }
};

} // namespace Utopia
} // namespace Sniper
