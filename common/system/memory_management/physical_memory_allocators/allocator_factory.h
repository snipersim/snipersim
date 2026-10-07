#pragma once

#include "simulator.h"
#include "config.hpp"

// NOTE!! buddy_policy.h should be included before reserve_thp.h
// common/system/memory_management/policies
#include "memory_management/policies/buddy_policy.h"
#include "memory_management/policies/reserve_thp_policy.h"
#include "memory_management/policies/baseline_policy.h"
#include "memory_management/policies/revelator_policy.h"
#include "memory_management/policies/revelator_simple_policy.h"
#include "memory_management/policies/revelator_thp_policy.h"
#include "memory_management/policies/numa_revelator_policy.h"

// include/memory_management/physical_memory_allocators/
#include "../../include/memory_management/physical_memory_allocators/reserve_thp.h"
#include "../../include/memory_management/physical_memory_allocators/baseline.h"
#include "../../include/memory_management/physical_memory_allocators/numa_reserve_thp.h"
#include "../../include/memory_management/physical_memory_allocators/revelator.h"
#include "../../include/memory_management/physical_memory_allocators/revelator_simple.h"
#include "../../include/memory_management/physical_memory_allocators/revelator_thp.h"
#include "../../include/memory_management/physical_memory_allocators/numa_revelator.h"
// PhysicalMemoryAllocator*
#include "../../include/memory_management/physical_memory_allocators/physical_memory_allocator.h"

#include "memory_management/numa/numa_policy.h"
#include <sstream>


using SniperBaselineAllocator = BaselineAllocator<Sniper::Baseline::MetricsPolicy>;
using SniperReserveTHPAllocator = ReservationTHPAllocator<Sniper::ReserveTHP::MetricsPolicy>;
using SniperNumaReserveTHPAllocator = NumaReservationTHPAllocator<Sniper::NumaReserveTHP::NumaReserveTHPSniperPolicy>;
using SniperRevelatorAllocator       = RevelatorAllocator<Sniper::Revelator::RevelatorSniperPolicy>;
using SniperRevelatorTHPAllocator    = RevelatorTHPAllocator<Sniper::RevelatorTHP::RevelatorTHPSniperPolicy>;
using SniperRevelatorSimpleAllocator = RevelatorSimpleAllocator<Sniper::RevelatorSimple::RevelatorSimpleSniperPolicy>;
using SniperNumaRevelatorAllocator   = NumaRevelatorAllocator<Sniper::NumaRevelator::NumaRevelatorSniperPolicy>;

// --------------------------------------------------------------------------
// Helper: parse a comma-separated integer list from a config string
// --------------------------------------------------------------------------
static inline std::vector<UInt64> parseCommaSeparatedUInt64(const String& raw)
{
    std::vector<UInt64> result;
    std::stringstream ss(raw.c_str());
    std::string token;
    while (std::getline(ss, token, ','))
    {
        // Trim whitespace
        size_t start = token.find_first_not_of(" \t");
        size_t end = token.find_last_not_of(" \t");
        if (start != std::string::npos)
            result.push_back(std::stoull(token.substr(start, end - start + 1)));
    }
    return result;
}

static inline NumaPolicy parseNumaPolicy(const String& raw)
{
    std::string s(raw.c_str());
    for (auto& c : s) c = std::tolower(c);
    if (s == "local")       return NumaPolicy::LOCAL;
    if (s == "bind")        return NumaPolicy::BIND;
    if (s == "interleave")  return NumaPolicy::INTERLEAVE;
    if (s == "preferred")   return NumaPolicy::PREFERRED;
    std::cout << "[MimicOS] Unknown NUMA policy: " << raw << ", defaulting to LOCAL" << std::endl;
    return NumaPolicy::LOCAL;
}

class AllocatorFactory
{
public:
    static PhysicalMemoryAllocator *createAllocator(String mimicos_name)
    {

        String allocator_name = Sim()->getCfg()->getString("perf_model/" + mimicos_name + "/memory_allocator_name");
        String allocator_type = Sim()->getCfg()->getString("perf_model/" + mimicos_name + "/memory_allocator_type");
        std::cout << "[MimicOS] [createAllocator] Creating physical memory allocator for " << mimicos_name <<
                     " - allocator_name = " << allocator_name <<
                    "  - allocator_type = " << allocator_type <<  std::endl;

        UInt64 memory_size = (UInt64)Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/memory_size");
        UInt64 kernel_size = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/kernel_size");

        std::cout << "[MimicOS] Kernel size in MB: " << kernel_size << std::endl;

        // The reserve_thp and baseline allocators register their statistics
        // under their type, so a virtualised run -- which gives the guest OS
        // an allocator of its own -- would register two "baseline" allocators
        // and abort on the duplicate.  The guest's keeps the host's name with
        // a _guest suffix (baseline_guest.total_allocations); the host's name
        // is unchanged, so no existing statistic moves.
        String stats_name = allocator_type;
        if (mimicos_name == "mimicos_guest")
            stats_name = allocator_type + "_guest";

        if (allocator_type == "reserve_thp")
        {
            // Based on FreeBSD's reservation-based THP allocator
            String frag_type = Sim()->getCfg()->getString("perf_model/" + allocator_name + "/frag_type");
            int max_order = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/max_order");
            float threshold_for_promotion = Sim()->getCfg()->getFloat("perf_model/" + allocator_name + "/threshold_for_promotion");
            return new SniperReserveTHPAllocator(stats_name, memory_size, max_order, kernel_size, frag_type, threshold_for_promotion);
        }
        else if (allocator_type == "baseline")
        {
            String frag_type = Sim()->getCfg()->getString("perf_model/" + allocator_name + "/frag_type");
            int max_order = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/max_order");
            return new SniperBaselineAllocator(stats_name, memory_size, max_order, kernel_size, frag_type);
        }
        else if (allocator_type == "numa_reserve_thp")
        { // NUMA-aware Reservation-based THP allocator
            String frag_type = Sim()->getCfg()->getString("perf_model/" + allocator_name + "/frag_type");
            int max_order = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/max_order");
            float threshold_for_promotion = Sim()->getCfg()->getFloat("perf_model/" + allocator_name + "/threshold_for_promotion");

            // NUMA-specific parameters
            UInt32 num_numa_nodes = (UInt32)Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/num_numa_nodes");
            NumaPolicy numa_policy = parseNumaPolicy(Sim()->getCfg()->getString("perf_model/" + allocator_name + "/numa_policy"));
            double utilization_threshold = Sim()->getCfg()->getFloat("perf_model/" + allocator_name + "/utilization_threshold");
            std::vector<UInt64> per_node_cap = parseCommaSeparatedUInt64(Sim()->getCfg()->getString("perf_model/" + allocator_name + "/per_node_capacity_mb"));
            std::vector<UInt64> per_node_kern = parseCommaSeparatedUInt64(Sim()->getCfg()->getString("perf_model/" + allocator_name + "/per_node_kernel_mb"));

            std::cout << "[MimicOS] Creating NUMA ReserveTHP: " << num_numa_nodes << " nodes, policy="
                      << numaPolicyToString(numa_policy) << std::endl;

            return new SniperNumaReserveTHPAllocator(
                allocator_name, memory_size, max_order, kernel_size, frag_type,
                threshold_for_promotion,
                num_numa_nodes, numa_policy, utilization_threshold,
                per_node_cap, per_node_kern);
        }
        else if (allocator_type == "revelator")
        { // Revelator: hash-placed frames a speculative engine can predict
            UInt64 rev_memory_size = (UInt64)Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/memory_size");
            String frag_type = Sim()->getCfg()->getString("perf_model/" + allocator_name + "/frag_type");
            int max_order = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/max_order");
            int number_of_hashes = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/number_of_hashes");
            double targ_frag = Sim()->getCfg()->getFloat("perf_model/" + allocator_name + "/target_fragmentation");
            bool enable_aggressive_swapouts = Sim()->getCfg()->getBool("perf_model/" + allocator_name + "/enable_aggressive_swapouts");
            int infrequency_threshold = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/infrequency_threshold");
            double hash1_usage_threshold = Sim()->getCfg()->getFloat("perf_model/" + allocator_name + "/hash1_usage_threshold");
            return new SniperRevelatorAllocator(allocator_name, max_order, kernel_size, frag_type,
                                                number_of_hashes, rev_memory_size, targ_frag,
                                                enable_aggressive_swapouts, infrequency_threshold,
                                                hash1_usage_threshold);
        }
        else if (allocator_type == "revelator_thp")
        { // Revelator with transparent huge pages
            UInt64 rev_memory_size = (UInt64)Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/memory_size");
            String frag_type = Sim()->getCfg()->getString("perf_model/" + allocator_name + "/frag_type");
            int max_order = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/max_order");
            int number_of_hashes = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/number_of_hashes");
            float threshold_for_promotion = Sim()->getCfg()->getFloat("perf_model/" + allocator_name + "/threshold_for_promotion");
            return new SniperRevelatorTHPAllocator(allocator_name, rev_memory_size, max_order, kernel_size,
                                                   frag_type, number_of_hashes, threshold_for_promotion);
        }
        else if (allocator_type == "revelator_simple")
        { // Revelator without the reservation machinery
            UInt64 rev_memory_size = (UInt64)Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/memory_size");
            String frag_type = Sim()->getCfg()->getString("perf_model/" + allocator_name + "/frag_type");
            int max_order = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/max_order");
            int number_of_hashes = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/number_of_hashes");
            return new SniperRevelatorSimpleAllocator(allocator_name, rev_memory_size, max_order,
                                                      kernel_size, frag_type, number_of_hashes);
        }
        else if (allocator_type == "numa_revelator")
        { // NUMA-aware Revelator (Kanellopoulos+, ISCA 2026)
            String frag_type = Sim()->getCfg()->getString("perf_model/" + allocator_name + "/frag_type");
            int max_order = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/max_order");
            int number_of_hashes = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/number_of_hashes");
            double targ_frag = Sim()->getCfg()->getFloat("perf_model/" + allocator_name + "/target_fragmentation");
            bool enable_aggressive_swapouts = Sim()->getCfg()->getBool("perf_model/" + allocator_name + "/enable_aggressive_swapouts");
            int infrequency_threshold = Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/infrequency_threshold");
            double hash1_usage_threshold = Sim()->getCfg()->getFloat("perf_model/" + allocator_name + "/hash1_usage_threshold");

            // NUMA-specific parameters
            UInt32 num_numa_nodes = (UInt32)Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/num_numa_nodes");
            NumaPolicy numa_policy = parseNumaPolicy(Sim()->getCfg()->getString("perf_model/" + allocator_name + "/numa_policy"));
            double utilization_threshold = Sim()->getCfg()->getFloat("perf_model/" + allocator_name + "/utilization_threshold");
            UInt32 max_speculation_nodes = (UInt32)Sim()->getCfg()->getInt("perf_model/" + allocator_name + "/max_speculation_nodes");
            std::vector<UInt64> per_node_cap = parseCommaSeparatedUInt64(Sim()->getCfg()->getString("perf_model/" + allocator_name + "/per_node_capacity_mb"));
            std::vector<UInt64> per_node_kern = parseCommaSeparatedUInt64(Sim()->getCfg()->getString("perf_model/" + allocator_name + "/per_node_kernel_mb"));

            std::cout << "[MimicOS] Creating NUMA Revelator: " << num_numa_nodes << " nodes, policy="
                      << numaPolicyToString(numa_policy) << std::endl;

            return new SniperNumaRevelatorAllocator(
                allocator_name, max_order, kernel_size, frag_type,
                number_of_hashes, memory_size, targ_frag,
                enable_aggressive_swapouts, infrequency_threshold, hash1_usage_threshold,
                num_numa_nodes, numa_policy, utilization_threshold, max_speculation_nodes,
                per_node_cap, per_node_kern);
        }
        else
        {
            std::cout << "[Sniper] Allocator type '" << allocator_type << "' not found" << std::endl;
            return nullptr;
        }
    }
};
