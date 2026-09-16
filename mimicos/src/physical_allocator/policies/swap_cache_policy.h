#pragma once
#include <iostream>

namespace Virtuoso {
namespace SwapCache {
    struct NoMetricsPolicy {
        template <typename T>
        void on_init(T*) {
            std::cout << "[Virtuoso] SwapCache initialized" << std::endl;
        }
    };
} // namespace SwapCache
} // namespace Virtuoso
