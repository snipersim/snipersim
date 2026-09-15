#pragma once
#include <fstream>
#include <string>
#include "stats.h"
#include "simulator.h"
#include "config.hpp"

namespace Sniper {
namespace Khugepaged {
    struct MetricsPolicy {
        mutable std::ofstream log_file;

        ~MetricsPolicy() { if (log_file.is_open()) log_file.close(); }

        template <typename T>
        void on_init(T* khp) {
            std::string fname = std::string(Sim()->getConfig()->getOutputDirectory().c_str())
                                + "/khugepaged.log";
            log_file.open(fname);

            auto& s = khp->getStats();
            registerStatsMetric("khugepaged", 0, "scans",             &s.scans);
            registerStatsMetric("khugepaged", 0, "regions_scanned",   &s.regions_scanned);
            registerStatsMetric("khugepaged", 0, "regions_promoted",  &s.regions_promoted);
            registerStatsMetric("khugepaged", 0, "pages_collapsed",   &s.pages_collapsed);
            registerStatsMetric("khugepaged", 0, "total_latency_ns",  &s.total_latency_ns);
        }

        void log(const std::string& msg) const
        { if (log_file.is_open()) log_file << msg << '\n'; }
    };
} // namespace Khugepaged
} // namespace Sniper
