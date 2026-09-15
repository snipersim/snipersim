#pragma once
#include <fstream>
#include "simulator.h"         // for Sim()
#include "mimicos.h"           // for swap behaviour if enabled
#include "stats.h"             // for registerStatsMetric


namespace Sniper{
    namespace Revelator{
        struct RevelatorSniperPolicy
        {
            // One logfile per policy (global to all allocators using this policy)
            static inline std::ofstream logfile;

            //
            // === 1. Stats Registration (Sniper-specific) ===
            //
            template<typename Alloc>
            static void register_stats(const String &name, Alloc &alloc)
            {
                auto &stats = alloc.get_hash_stats();
                int num     = alloc.get_number_of_hashes();

                for (int i = 0; i < num; i++)
                {
                    registerStatsMetric("revelator_allocator", 0,
                                        "hash" + itostr(i+1) + "_hits",
                                        &stats.hits[i]);

                    registerStatsMetric("revelator_allocator", 0,
                                        "hash" + itostr(i+1) + "_tries",
                                        &stats.tries[i]);

                    registerStatsMetric("revelator_allocator", 0,
                                        "hash" + itostr(i+1) + "_swap_attempts",
                                        &stats.swap_attempts[i]);

                    registerStatsMetric("revelator_allocator", 0,
                                        "hash" + itostr(i+1) + "_swap_attempts_same_address",
                                        &stats.swap_attempts_same_address[i]);

                    registerStatsMetric("revelator_allocator", 0,
                                        "hash" + itostr(i+1) + "_successful_pt_allocations",
                                        &stats.successful_pt_allocations[i]);
                }
            }

            //
            // === 2. Initialize (Sniper-specific logging setup) ===
            //
            template<typename Alloc>
            static void on_init(const String &name,
                                UInt64 memory_size,
                                UInt64 kernel_size,
                                int max_order,
                                const String &frag_type,
                                Alloc *alloc)
            {
                // Create logfile inside Sniper output directory
                std::string fname = std::string(name.c_str()) + ".revelator_allocator.log";
                std::string fullpath = std::string(Sim()->getConfig()->getOutputDirectory().c_str())
                                    + "/" + fname;

                logfile.open(fullpath.c_str());
                alloc->log_stream = &logfile;   // attach to allocator
            }

            //
            // === 3. Swap-mode enabling logic (unchanged) ===
            //
            template<typename Alloc>
            static void maybe_enable_swap_mode(Alloc &alloc)
            {
                if (alloc.is_swap_mode_enabled()) return;
                if (alloc.get_number_of_hashes() <= 0) return;
                if (alloc.number_of_pages_used == 0) return;

                auto &stats = alloc.get_hash_stats();
                double tries0 = (double) stats.tries[0];
                if (tries0 == 0) return;

                double ratio = (double) stats.hits[0] / tries0;

                if (ratio < alloc.hash1_usage_threshold &&
                    alloc.aggressive_swapouts_enabled())
                {
                    alloc.enable_swap_mode();
                }
            }

            //
            // === 4. Decide whether we *should* try a victim swap ===
            //
            template<typename Alloc>
            static bool should_attempt_swap(const Alloc &alloc,
                                            int hash_index,
                                            bool is_pagetable_allocation,
                                            const typename Alloc::AllocationEntry &entry)
            {
                return alloc.is_swap_mode_enabled() &&
                    (hash_index == 1 || hash_index == 2) &&     // original behavior
                    !entry.is_poisoned &&
                    !is_pagetable_allocation;
            }

            //
            // === 5. Swap out a victim using MimicOS interface ===
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

                // Check page temperature
                bool cold = (mos->getPageTable(app_id)->getAccessesPerVPN(victim_va >> 12)
                            < static_cast<UInt64>(alloc.get_infrequency_threshold()));

                if (!mos->isSwapEnabled() || !cold)
                    return false;

                stats.swap_attempts[stat_index]++;

                if (mos->swapOutPage(victim_va, app_id)) {
                    mos->deletePageTableEntry(victim_va, app_id);

                    // free the slot
                    entry.is_free                 = true;
                    entry.address                 = (IntPtr)-1;
                    entry.app_id                  = (UInt64)-1;
                    entry.is_pagetable_allocation = false;

                    alloc.number_of_pages_used--;
                    return true;
                }

                return false;
            }
        };
    }
}   