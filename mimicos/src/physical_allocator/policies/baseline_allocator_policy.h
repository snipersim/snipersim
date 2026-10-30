#pragma once
#include <iostream>
#include <string>

namespace Virtuoso {
    namespace Baseline {
        struct NoMetricsPolicy
        {
            /* called from BaselineAllocator ctor */
            template <typename Alloc>
            void on_init(const String& /*name*/,
                         int /*mem*/,
                         int /*kernel*/,
                         Alloc* /*alloc*/)
            {
                std::cout << "[VirtuOS] Baseline Allocator" << std::endl;
            }

            /* BaselineAllocator inherits privately, so `log()` becomes `this->log()` */
            void log(const std::string& msg) const
            { std::cout << msg << '\n'; }
        };

    } // namespace Baseline
} // namespace Virtuoso

