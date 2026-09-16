#pragma once
#include <iostream>
#include <string>

namespace Virtuoso {
    namespace LinuxBuddyAnon {
        struct NoMetricsPolicy
        {
            template <typename Alloc>
            void on_init(const String& name,
                         int /*mem*/,
                         int /*kernel*/,
                         Alloc* /*alloc*/)
            {
                std::cout << "[VirtuOS] LinuxBuddyAnon Allocator (per-CPU pageset + buddy fallback)" << std::endl;
            }

            void log(const std::string& msg) const
            { std::cout << msg << '\n'; }
        };
    }
}
