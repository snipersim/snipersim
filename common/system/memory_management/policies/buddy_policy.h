#pragma once
#include "stats.h"
#include "simulator.h"
#include "config.hpp"
#include <fstream>
#include <iostream>

#include "memory_management/physical_memory_allocators/buddy_policy_traits.h"

#include "memory_management/policies/reserve_thp_policy.h"
#include "memory_management/policies/baseline_policy.h"
#include "memory_management/policies/numa_reserve_thp_policy.h"
#include "memory_management/policies/numa_revelator_policy.h"

namespace Sniper {
    namespace Buddy {
        struct MetricsPolicy
        {
            mutable std::ofstream log_file;

            ~MetricsPolicy() {
                if (log_file.is_open())
                    log_file.close();
            }

            void on_init(int mem_size, int max_order, int kernel_size)
            {
                std::string log_file_name =  std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/buddy.log";
                log_file.open(log_file_name);
                if (!log_file.is_open()) {
                    throw std::runtime_error("[BUDDY] Failed to open log file");
                }

                log_file << "[Buddy] Init: mem_size " << mem_size << "KB" <<
                             " max_order = " << max_order <<
                             " kernel size = " << kernel_size << "KB" << std::endl;
            }

            void on_out_of_memory(UInt64 bytes, UInt64 addr, UInt64 core)
            {
                log_file << "[Buddy] Out of memory for " << bytes << " bytes" <<
                             " addr = " << addr <<
                             " core = " << core << std::endl;
            }

            void on_fragmentation_done()
            {
                log_file << "[Buddy] Fragmentation complete" << std::endl;
            }

            /* Logging */
            void log(const std::string &msg) const {
                log_file << msg << std::endl;
            }
        };
    }

    namespace ReserveTHP {
        struct MetricsPolicy; // forward declared elsewhere
    }
}

template <>
struct BuddyPolicyFor<Sniper::ReserveTHP::MetricsPolicy> {
    using type = Sniper::Buddy::MetricsPolicy;
};

template <>
struct BuddyPolicyFor<Sniper::Baseline::MetricsPolicy> {
    using type = Sniper::Buddy::MetricsPolicy;
};

template <>
struct BuddyPolicyFor<Sniper::NumaReserveTHP::NumaReserveTHPSniperPolicy> {
    using type = Sniper::Buddy::MetricsPolicy;
};

template <>
struct BuddyPolicyFor<Sniper::NumaRevelator::NumaRevelatorSniperPolicy> {
    using type = Sniper::Buddy::MetricsPolicy;
};
