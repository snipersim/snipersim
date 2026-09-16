#pragma once
#include <iostream>

namespace Virtuoso {
namespace HugeTLBfs {
    struct NoMetricsPolicy {
        template <typename T>
        void on_init(T*) {
            std::cout << "[Virtuoso] HugeTLBfs initialized" << std::endl;
        }
    };
} // namespace HugeTLBfs
} // namespace Virtuoso
