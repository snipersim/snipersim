#pragma once

#include <iostream>
#include "fixed_types.h"

#include "memory_management/physical_memory_allocators/buddy_policy_traits.h"

/* Policies that depend on Buddy Policy */
#include "physical_allocator/policies/reserve_thp_policy.h"
#include "physical_allocator/policies/baseline_allocator_policy.h"
#include "physical_allocator/policies/linux_buddy_anon_policy.h"

namespace Virtuoso {
    namespace Buddy {
        struct NoMetricsPolicy
        {
            void on_init(int mem_size, int max_order, int kernel_size)
            {
                std::cout << "[Buddy] Init: mem_size " << mem_size << "KB" <<
                             " max_order = " << max_order <<
                             " kernel size = " << kernel_size << "KB" << std::endl;
            }

            void on_out_of_memory(UInt64 bytes, UInt64 addr, UInt64 core)
            {
                std::cout << "[Buddy] Out of memory for " << bytes << " bytes" << 
                             " addr = " << addr <<
                             " core = " << core << std::endl;
            }
            void on_fragmentation_done() {
                std::cout << "[Buddy] Fragmentation done\n"; 
            }

            /* Logging */
            void log(const std::string &msg) const { 
                /* std::cout; no log_file */
                std::cout << msg << std::endl;
            }
        };

        namespace ReserveTHP {
            struct NoMetricsPolicy; // forward declared elsewhere
        }
        namespace LinuxBuddyAnon {
            struct NoMetricsPolicy; // forward declared elsewhere
        }
    }
}

// Specialize the mapping here (after full definition of Virtuoso::Buddy::NoMetricsPolicy)
template <>
struct BuddyPolicyFor<Virtuoso::ReserveTHP::NoMetricsPolicy> {
    using type = Virtuoso::Buddy::NoMetricsPolicy;
};

template <>
struct BuddyPolicyFor<Virtuoso::Baseline::NoMetricsPolicy> {
    using type = Virtuoso::Buddy::NoMetricsPolicy;
};

template <>
struct BuddyPolicyFor<Virtuoso::LinuxBuddyAnon::NoMetricsPolicy> {
    using type = Virtuoso::Buddy::NoMetricsPolicy;
};

