#pragma once

#include <iostream>
#include <sstream>
#include <string>
#include "fixed_types.h"


// We use a template-based policy here to allow for flexibility and extensibility in the design of the ReservationTHPAllocator.
// This way, we can easily swap out different policies for metrics collection or logging without changing the core logic of the allocator.
// This is particularly useful when we want to (i) try embedding a policy inside the simulator (e.g., Sniper - we internally call this "sniperspace")
//  or (ii) have the userspace MimicOS handle the policy (e.g., for simulating different OS behaviors - we internally call this "userspace").

namespace Virtuoso {
    namespace ReserveTHP {
        struct NoMetricsPolicy
        {
            template <typename Allocator>
            void on_init(const String& /*name*/, int /*memory_size*/, int /*kernel_size*/, int threshold_for_promotion, Allocator* /*phys_mem_alloc*/) {
                std::cout << "[MimicOS] Reservation-based THP Allocator" << std::endl;
                std::cout << "[MimicOS] ReserveTHP: threshold_for_promotion = " << threshold_for_promotion << std::endl;
            }

            /* Logging */
            void log(const std::string &msg) const { 
                /* std::cout; no log_file */
                std::cout << msg << std::endl;
            }

            template <typename... Args>
            void log(Args&&... args) const {
                std::ostringstream oss;
                (oss << ... << std::forward<Args>(args));
                std::cout << oss.str() << std::endl;
            }
        };
    }
}