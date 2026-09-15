#pragma once
#include <fstream>
#include <string>
#include "stats.h"
#include "simulator.h"
#include "config.hpp"

namespace Sniper {
namespace Kswapd {
    struct MetricsPolicy {
        mutable std::ofstream log_file;

        ~MetricsPolicy() { if (log_file.is_open()) log_file.close(); }

        template <typename T>
        void on_init(T* ks) {
            std::string fname = std::string(Sim()->getConfig()->getOutputDirectory().c_str())
                                + "/kswapd.log";
            log_file.open(fname);

            auto& s = ks->getStats();
            registerStatsMetric("kswapd", 0, "scans",              &s.scans);
            registerStatsMetric("kswapd", 0, "pages_reclaimed",    &s.pages_reclaimed);
            registerStatsMetric("kswapd", 0, "pages_demoted",      &s.pages_demoted);
            registerStatsMetric("kswapd", 0, "pages_promoted",     &s.pages_promoted);
            registerStatsMetric("kswapd", 0, "direct_reclaims",    &s.direct_reclaims);
            registerStatsMetric("kswapd", 0, "oom_events",         &s.oom_events);
            registerStatsMetric("kswapd", 0, "active_list_size",   &s.active_list_size);
            registerStatsMetric("kswapd", 0, "inactive_list_size", &s.inactive_list_size);
        }

        void log(const std::string& msg) const
        { if (log_file.is_open()) log_file << msg << '\n'; }
    };
} // namespace Kswapd
} // namespace Sniper
