#pragma once
#include "stats.h"
#include "simulator.h"
#include "config.hpp"
#include "sim_log.h"
#include "mimicos.h"
#include "core_manager.h"
#include "core.h"
#include "thread.h"
#include "debug_config.h"
#include "memory_management/misc/vma.h"
#include <tuple>

namespace Sniper {
    namespace Spot {
        /*
         * The Sniper side of the SpOT allocator: its statistics, its log
         * (SimLog under DEBUG_SPOT_ALLOCATOR) and the OS's VMA bookkeeping,
         * which the allocator reaches through the faulting core.  The
         * allocator's allocate() is handed the core id (that is what every
         * exception handler passes), and a VMA belongs to an application,
         * so the application is the one the core's thread runs.
         */
        struct SpotSniperPolicy
        {
            mutable SimLog* m_log = nullptr;

            ~SpotSniperPolicy() { delete m_log; }

            template <typename Alloc>
            void on_init(const String& name, UInt64 memory_size, UInt64 kernel_size,
                         int max_order, const String& frag_type, Alloc* alloc)
            {
                m_log = new SimLog("SpOTAllocator", -1, DEBUG_SPOT_ALLOCATOR);
                std::cout << "[MimicOS] SpOT Allocator: " << memory_size << " MB, kernel " << kernel_size
                          << " MB, max order " << max_order << ", fragmentation " << frag_type << std::endl;

                auto& s = alloc->getStats();
                registerStatsMetric(name, 0, "vmas_placed", &s.vmas_placed);
                registerStatsMetric(name, 0, "vmas_placed_in_largest", &s.vmas_placed_in_largest);
                registerStatsMetric(name, 0, "offset_allocations", &s.offset_allocations);
                registerStatsMetric(name, 0, "fallback_allocations", &s.fallback_allocations);
                registerStatsMetric(name, 0, "no_vma_allocations", &s.no_vma_allocations);
            }

            template <typename Alloc>
            void log(Alloc*, const std::string& msg) const
            {
                if (m_log) m_log->debug(msg);
            }

            static int appIdOfCore(UInt64 core_id)
            {
                Core* core = Sim()->getCoreManager()->getCoreFromID(core_id);
                Thread* thread = core ? core->getThread() : NULL;
                return thread ? thread->getAppId() : -1;
            }

            // The VMA containing the address: (index, physical offset or -1
            // when the VMA has not been placed yet, size in bytes); index
            // -1 when the address lies in no VMA.  Placing marks the VMA
            // allocated.
            template <typename Alloc>
            static std::tuple<int, int64_t, IntPtr> find_VMA_specs(IntPtr address, UInt64 core_id, Alloc*)
            {
                int app_id = appIdOfCore(core_id);
                if (app_id < 0)
                    return std::make_tuple(-1, static_cast<int64_t>(-1), static_cast<IntPtr>(0));
                auto& vma_list = Sim()->getMimicOS()->getVMA(app_id);
                for (size_t i = 0; i < vma_list.size(); i++)
                {
                    VMA& vma = vma_list[i];
                    if (!vma.contains(address))
                        continue;
                    int64_t offset = static_cast<int64_t>(-1);
                    if (vma.isAllocated())
                        offset = static_cast<int64_t>(vma.getPhysicalOffset());
                    else
                        Sim()->getMimicOS()->setAllocatedVMA(app_id, i);
                    return std::make_tuple(static_cast<int>(i), offset, vma.getEnd() - vma.getBase());
                }
                return std::make_tuple(-1, static_cast<int64_t>(-1), static_cast<IntPtr>(0));
            }

            template <typename Alloc>
            static void setPhysicalOffset(UInt64 core_id, int vma_index, int64_t offset, Alloc*)
            {
                int app_id = appIdOfCore(core_id);
                if (app_id >= 0)
                    Sim()->getMimicOS()->setPhysicalOffset(app_id, vma_index, static_cast<IntPtr>(offset));
            }

            template <typename Alloc>
            static void incrementSuccessfulOffsetBasedAllocations(UInt64 core_id, IntPtr address, Alloc*)
            {
                int app_id = appIdOfCore(core_id);
                if (app_id >= 0)
                    Sim()->getMimicOS()->incrementSuccessfulOffsetBasedAllocations(app_id, address);
            }
        };
    }
}
