#pragma once
#include <fstream>
#include <string>
#include "stats.h"
#include "simulator.h"
#include "config.hpp"

namespace Sniper {
namespace Kcompactd {
    struct MetricsPolicy {
        mutable std::ofstream log_file;

        ~MetricsPolicy() { if (log_file.is_open()) log_file.close(); }

        template <typename T>
        void on_init(T* kcompactd) {
            std::string fname = std::string(Sim()->getConfig()->getOutputDirectory().c_str())
                                + "/kcompactd.log";
            log_file.open(fname);

            auto& s = kcompactd->getStats();
            registerStatsMetric("kcompactd", 0, "scans",             &s.scans);
            registerStatsMetric("kcompactd", 0, "pages_migrated",    &s.pages_migrated);
            registerStatsMetric("kcompactd", 0, "regions_freed",     &s.regions_freed);
            registerStatsMetric("kcompactd", 0, "regions_scanned",   &s.regions_scanned);
            registerStatsMetric("kcompactd", 0, "failed_migrations", &s.failed_migrations);
            registerStatsMetric("kcompactd", 0, "total_latency_ns",  &s.total_latency_ns);
        }

        void log(const std::string& msg) const
        { if (log_file.is_open()) log_file << msg << '\n'; }
    };
} // namespace Kcompactd
} // namespace Sniper
