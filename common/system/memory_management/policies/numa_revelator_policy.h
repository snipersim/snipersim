#pragma once
#include <fstream>
#include "simulator.h"
#include "mimicos.h"
#include "stats.h"

namespace Sniper {
    namespace NumaRevelator {
        struct NumaRevelatorSniperPolicy
        {
            static inline std::ofstream logfile;

            //
            // === 1. Stats Registration (NUMA-aware) ===
            //
            template<typename Alloc>
            static void register_stats(const String &name, Alloc &alloc)
            {
                auto &stats = alloc.get_hash_stats();
                int num = alloc.get_number_of_hashes();

                for (int i = 0; i < num; i++)
                {
                    registerStatsMetric("numa_revelator_allocator", 0,
                                        "hash" + itostr(i+1) + "_hits",
                                        &stats.hits[i]);
                    registerStatsMetric("numa_revelator_allocator", 0,
                                        "hash" + itostr(i+1) + "_tries",
                                        &stats.tries[i]);
                    registerStatsMetric("numa_revelator_allocator", 0,
                                        "hash" + itostr(i+1) + "_swap_attempts",
                                        &stats.swap_attempts[i]);
                    registerStatsMetric("numa_revelator_allocator", 0,
                                        "hash" + itostr(i+1) + "_swap_attempts_same_address",
                                        &stats.swap_attempts_same_address[i]);
                    registerStatsMetric("numa_revelator_allocator", 0,
                                        "hash" + itostr(i+1) + "_successful_pt_allocations",
                                        &stats.successful_pt_allocations[i]);
                }

                // NUMA-specific stats
                UInt32 num_nodes = alloc.getNumNumaNodes();
                for (UInt32 n = 0; n < num_nodes; ++n)
                {
                    registerStatsMetric("numa_revelator_allocator", 0,
                                        "node" + itostr(n) + "_allocs",
                                        &alloc.getNumaNodeAllocStats()[n]);
                    registerStatsMetric("numa_revelator_allocator", 0,
                                        "node" + itostr(n) + "_spills",
                                        &alloc.getNumaNodeSpillStats()[n]);
                }

                registerStatsMetric("numa_revelator_allocator", 0,
                                    "local_allocs", &alloc.getLocalAllocCount());
                registerStatsMetric("numa_revelator_allocator", 0,
                                    "bind_allocs", &alloc.getBindAllocCount());
                registerStatsMetric("numa_revelator_allocator", 0,
                                    "interleave_allocs", &alloc.getInterleaveAllocCount());
            }

            //
            // === 2. Initialize (logging setup) ===
            //
            template<typename Alloc>
            static void on_init(const String &name,
                                UInt64 memory_size,
                                UInt64 kernel_size,
                                int max_order,
                                const String &frag_type,
                                Alloc *alloc)
            {
                std::string fname = std::string(name.c_str()) + ".numa_revelator_allocator.log";
                std::string fullpath = std::string(Sim()->getConfig()->getOutputDirectory().c_str())
                                    + "/" + fname;
                logfile.open(fullpath.c_str());
                alloc->log_stream = &logfile;

                std::cout << "[MimicOS] NUMA Revelator Allocator initialized: "
                          << alloc->getNumNumaNodes() << " nodes" << std::endl;
            }

            //
            // === 3. Swap-mode enabling (same logic, NUMA-aware) ===
            //
            template<typename Alloc>
            static void maybe_enable_swap_mode(Alloc &alloc)
            {
                if (alloc.is_swap_mode_enabled()) return;
                if (alloc.get_number_of_hashes() <= 0) return;
                if (alloc.number_of_pages_used == 0) return;

                auto &stats = alloc.get_hash_stats();
                double tries0 = (double)stats.tries[0];
                if (tries0 == 0) return;

                double ratio = (double)stats.hits[0] / tries0;
                if (ratio < alloc.hash1_usage_threshold &&
                    alloc.aggressive_swapouts_enabled())
                {
                    alloc.enable_swap_mode();
                }
            }

            //
            // === 4. Should attempt swap ===
            //
            template<typename Alloc>
            static bool should_attempt_swap(const Alloc &alloc,
                                            int hash_index,
                                            bool is_pagetable_allocation,
                                            const typename Alloc::AllocationEntry &entry)
            {
                return alloc.is_swap_mode_enabled() &&
                    (hash_index == 1 || hash_index == 2) &&
                    !entry.is_poisoned &&
                    !is_pagetable_allocation;
            }

            //
            // === 5. Try swap out victim ===
            //
            template<typename Alloc>
            static bool try_swap_out_victim(Alloc &alloc,
                                            IntPtr hash_idx,
                                            IntPtr new_address,
                                            UInt64 app_id,
                                            typename Alloc::AllocationEntry &entry,
                                            int stat_index)
            {
                auto &stats = alloc.get_hash_stats();

                IntPtr victim_va = entry.address;
                if (victim_va == new_address) {
                    stats.swap_attempts_same_address[stat_index]++;
                    return false;
                }

                if (entry.is_free)
                    return false;

                auto *mos = Sim()->getMimicOS();

                bool cold = (mos->getPageTable(app_id)->getAccessesPerVPN(victim_va >> 12)
                            < static_cast<UInt64>(alloc.get_infrequency_threshold()));

                if (!mos->isSwapEnabled() || !cold)
                    return false;

                stats.swap_attempts[stat_index]++;

                if (mos->swapOutPage(victim_va, app_id)) {
                    mos->deletePageTableEntry(victim_va, app_id);

                    entry.is_free = true;
                    entry.address = (IntPtr)-1;
                    entry.app_id = (UInt64)-1;
                    entry.is_pagetable_allocation = false;

                    alloc.number_of_pages_used--;

                    // Also decrement per-node page count
                    alloc.decrementNodePageUsed(entry.node_id);

                    return true;
                }

                return false;
            }
        };
    }
}
