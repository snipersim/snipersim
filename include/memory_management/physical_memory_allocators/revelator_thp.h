#pragma once

#include "physical_memory_allocator.h"
#include "revelator_types.h"
#include <vector>
#include <map>
#include <unordered_map>
#include <bitset>
#include <tuple>
#include <fstream>
#include <cassert>
#include <algorithm>
#include <city.h>

/**
 * @class RevelatorTHPAllocator
 * @brief Template-based hybrid allocator following a 4-tier allocation strategy:
 *
 *   1. **2MB Revelator Hash** – Hash virtual 2MB region ID into physical 2MB
 *      space; if the entire physical region is free, allocate it as a single
 *      2MB huge page.
 *   2. **Conventional THP** – Linear-scan for a free 2MB physical region,
 *      reserve it, track sub-pages via bitset; promote when utilization
 *      exceeds threshold.
 *   3. **4KB Revelator Hash** – Hash virtual page address into 4KB physical
 *      space; succeed only if backing 2MB region state allows it.
 *   4. **Fallback linear scan** – Find any free 4KB page not inside a
 *      promoted or Revelator-2MB region.
 *
 * Kernel / page-table pages follow a separate 2-stage path:
 *   K1. Hash-based placement in the kernel pool.
 *   K2. Linear-scan fallback over the kernel pool.
 *
 * Template parameter Policy provides Sniper-specific hooks:
 *   - register_stats(name, alloc)
 *   - on_init(name, memory_size, kernel_size, max_order, frag_type, alloc_ptr)
 *   - on_allocate_feedback(core_id, general_stats, thp_stats)
 *   - get_thp_fragmentation_target(name)
 */
template<typename Policy>
class RevelatorTHPAllocator : public PhysicalMemoryAllocator, private Policy
{
public:
    // ── Stat structs ─────────────────────────────────────────────────────
    struct HashStats {
        UInt64 *successful_data_allocations;   // per-hash successful data allocations
        UInt64 *sucessful_pt_allocations;      // per-hash successful PT allocations
        UInt64 total_hash_data_allocations;
        UInt64 total_hash_pt_allocations;
    };

    struct GeneralStats {
        UInt64 total_page_table_allocations;
        UInt64 total_data_allocations;
        UInt64 data_allocated_fallback;
        UInt64 pt_allocated_fallback;
        UInt64 revelator_4kb_data_allocated;
        UInt64 revelator_2mb_data_allocated;
    };

    struct THPStats {
        UInt64 two_mb_reserved;
        UInt64 two_mb_promoted;
        UInt64 two_mb_demoted;
        UInt64 total_thp_allocations;
    };

    // ── Constructor ──────────────────────────────────────────────────────
    RevelatorTHPAllocator(String name,
                          UInt64 memory_size,
                          int    max_order,
                          UInt64 kernel_size,
                          String frag_type,
                          int    number_of_hashes,
                          float  threshold_for_promotion)
        : PhysicalMemoryAllocator(name, memory_size, kernel_size)
        , m_max_order(max_order)
        , m_number_of_hashes(number_of_hashes)
        , m_threshold_for_promotion(threshold_for_promotion)
    {
        m_memory_size = memory_size;
        m_kernel_size = kernel_size;

        m_kernel_size_in_pages = kernel_size * 1024 / 4;
        m_total_pages = memory_size * 1024 / 4 - m_kernel_size_in_pages;
        number_of_pages_used = 0;
        faults = 0;

        // --- Kernel page tracking ---
        m_kernel_physical_map.assign(m_kernel_size_in_pages, 0);

        // --- Data page tracking ---
        m_four_kb_physical_map.assign(m_total_pages, 0);

        // --- 2MB region tracking ---
        UInt64 num_2mb_regions = m_total_pages / PAGES_IN_2MB_REGION;
        m_two_mb_physical_map.assign(num_2mb_regions, RegionState::Free);

        // --- Poisoning arrays ---
        m_poisoned_pages.assign(m_total_pages, false);
        m_poisoned_pages_kernel.assign(m_kernel_size_in_pages, false);

        faults++;

        // --- Hash stats ---
        m_hash_stats.successful_data_allocations = new UInt64[m_number_of_hashes]();
        m_hash_stats.sucessful_pt_allocations    = new UInt64[m_number_of_hashes]();
        m_hash_stats.total_hash_data_allocations = 0;
        m_hash_stats.total_hash_pt_allocations   = 0;

        // --- General stats ---
        m_general_stats = {0, 0, 0, 0, 0, 0};

        // --- THP stats ---
        m_thp_stats = {0, 0, 0, 0};

        // --- Policy hooks ---
        Policy::register_stats(name, *this);
        Policy::on_init(name, memory_size, kernel_size, max_order, frag_type, this);
    }

    ~RevelatorTHPAllocator()
    {
        delete[] m_hash_stats.successful_data_allocations;
        delete[] m_hash_stats.sucessful_pt_allocations;
    }

    // ── Accessors for Policy ─────────────────────────────────────────────
    int get_number_of_hashes() const { return m_number_of_hashes; }

    HashStats&       get_hash_stats()       { return m_hash_stats; }
    const HashStats& get_hash_stats() const { return m_hash_stats; }

    GeneralStats&       get_general_stats()       { return m_general_stats; }
    const GeneralStats& get_general_stats() const { return m_general_stats; }

    THPStats&       get_thp_stats()       { return m_thp_stats; }
    const THPStats& get_thp_stats() const { return m_thp_stats; }

    float getAllocRatioForHash(int h) override
    {
        if (m_hash_stats.successful_data_allocations[h] == 0)
            return 0.0f;
        UInt64 total_tried = m_general_stats.total_data_allocations;
        if (total_tried == 0) return 0.0f;
        return (float)(m_hash_stats.successful_data_allocations[h]) / (float)(total_tried);
    }

    // ══════════════════════════════════════════════════════════════════════
    //  MAIN ALLOCATION ENTRY POINT
    // ══════════════════════════════════════════════════════════════════════
    std::pair<UInt64, UInt64> allocate(UInt64 size,
                                       IntPtr address,
                                       UInt64 core_id,
                                       bool is_pagetable_allocation,
                                       bool is_instruction_allocation = false) override
    {
        if (is_instruction_allocation)
            return this->allocateInstruction(size);

        faults++;

        // --- Spec-engine feedback (Policy hook) ---
        Policy::on_allocate_feedback(core_id, m_general_stats, m_thp_stats);

        // --- Kernel / page-table allocation ---
        if (is_pagetable_allocation) {
            m_general_stats.total_page_table_allocations++;
            return allocateKernelPage(address);
        }

        m_general_stats.total_data_allocations++;

        // ── STAGE 1: 2MB Revelator hash ─────────────────────────────────
        auto revelator_huge_result = tryRevelatorHugePageAllocation(address);
        if (revelator_huge_result.first != (UInt64)-1)
            return revelator_huge_result;

        // ── STAGE 2: Conventional THP ───────────────────────────────────
        auto thp_result = tryThpAllocation(address, core_id);
        if (thp_result.first != (UInt64)-1) {
            m_thp_stats.total_thp_allocations++;
            return thp_result;
        }

        // ── STAGE 3: 4KB Revelator hash ─────────────────────────────────
        auto revelator_4kb_result = tryRevelatorHash(address);
        if (revelator_4kb_result.first != (UInt64)-1) {
            m_hash_stats.total_hash_data_allocations++;
            return revelator_4kb_result;
        }

        // ── STAGE 4: Fallback linear scan ────────────────────────────────
        return fallbackScan(address);
    }

    // ══════════════════════════════════════════════════════════════════════
    //  KERNEL PAGE ALLOCATION
    // ══════════════════════════════════════════════════════════════════════
    std::pair<UInt64, UInt64> allocateKernelPage(UInt64 address)
    {
        const UInt64 to_hash = address >> PAGE_SIZE_2MB_BITS;

        // Stage K1: Hash-based placement
        for (int i = 1; i <= m_number_of_hashes; ++i) {
            UInt64 hash_index = hashFunction(i * to_hash, m_kernel_size_in_pages);

            if (!m_poisoned_pages_kernel[hash_index] && !m_kernel_physical_map[hash_index]) {
                m_hash_stats.sucessful_pt_allocations[i - 1]++;
                m_hash_stats.total_hash_pt_allocations++;
                m_kernel_physical_map[hash_index] = 1;
                number_of_pages_used++;
                return { hash_index, (UInt64)PAGE_SIZE_4KB_BITS };
            }
        }

        // Stage K2: Linear-scan fallback
        for (UInt64 i = 0; i < m_kernel_size_in_pages; ++i) {
            if (!m_kernel_physical_map[i]) {
                m_kernel_physical_map[i] = 1;
                number_of_pages_used++;
                m_general_stats.pt_allocated_fallback++;
                return { i, (UInt64)PAGE_SIZE_4KB_BITS };
            }
        }

        // OOM
        log_file << "[RevelatorTHP] OOM (kernel): Could not allocate for VA "
                 << address << "\n";
        assert(false);
        return { (UInt64)-1, (UInt64)PAGE_SIZE_4KB_BITS };
    }

    // ══════════════════════════════════════════════════════════════════════
    //  STAGE 1: 2MB Revelator Hash
    // ══════════════════════════════════════════════════════════════════════
    std::pair<UInt64, UInt64> tryRevelatorHugePageAllocation(UInt64 address)
    {
        const UInt64 region_2MB_va_id = address >> PAGE_SIZE_2MB_BITS;
        const UInt64 num_physical_regions = m_total_pages / PAGES_IN_2MB_REGION;

        for (int i = 1; i <= m_number_of_hashes; ++i) {
            UInt64 target_region_idx = hashFunction(i * region_2MB_va_id, num_physical_regions);

            if (m_two_mb_physical_map[target_region_idx] == RegionState::Free) {
                // Allocate the entire 2MB region
                m_two_mb_physical_map[target_region_idx] = RegionState::ReservedRevelator;

                UInt64 base_4kb_idx = target_region_idx * PAGES_IN_2MB_REGION;
                for (int j = 0; j < PAGES_IN_2MB_REGION; ++j) {
                    assert(m_four_kb_physical_map[base_4kb_idx + j] == 0);
                    m_four_kb_physical_map[base_4kb_idx + j] = 1;
                }
                number_of_pages_used += PAGES_IN_2MB_REGION;

                m_general_stats.revelator_2mb_data_allocated++;

                UInt64 phys_addr = base_4kb_idx + m_kernel_size_in_pages;
                return { phys_addr, (UInt64)PAGE_SIZE_2MB_BITS };
            }
        }

        return { (UInt64)-1, (UInt64)0 };
    }

    // ══════════════════════════════════════════════════════════════════════
    //  STAGE 2: Conventional THP (reserve 2MB, track sub-pages, promote)
    // ══════════════════════════════════════════════════════════════════════
    std::pair<UInt64, UInt64> tryThpAllocation(UInt64 address, UInt64 core_id)
    {
        const UInt64 region_2MB_id = address >> PAGE_SIZE_2MB_BITS;
        auto it = m_two_mb_map.find(region_2MB_id);

        // If this virtual 2MB region is not yet tracked, find a physical region
        if (it == m_two_mb_map.end()) {
            bool found_free_region = false;

            for (UInt64 i = 0; i < (m_total_pages / PAGES_IN_2MB_REGION); i++) {
                if (m_two_mb_physical_map[i] == RegionState::Free) {
                    UInt64 region_start_pa = i * PAGES_IN_2MB_REGION + m_kernel_size_in_pages;

                    m_two_mb_physical_map[i] = RegionState::ReservedTHP;

                    auto result = m_two_mb_map.emplace(region_2MB_id,
                        std::make_tuple(region_start_pa,
                                        std::bitset<PAGES_IN_2MB_REGION>(),
                                        false));
                    it = result.first;

                    m_thp_stats.two_mb_reserved++;
                    found_free_region = true;
                    break;
                }
            }
            if (!found_free_region)
                return { (UInt64)-1, (UInt64)0 };
        }

        auto& region_info = it->second;
        auto& bitset      = std::get<1>(region_info);
        const UInt64 base_physical_page = std::get<0>(region_info);
        const int offset_in_2MB = (address >> PAGE_SIZE_4KB_BITS) & (PAGES_IN_2MB_REGION - 1);

        // Defensive: already promoted → return 2MB mapping
        if (std::get<2>(region_info))
            return { base_physical_page, (UInt64)PAGE_SIZE_2MB_BITS };

        // Mark specific 4KB sub-page
        if (!bitset.test(offset_in_2MB)) {
            bitset.set(offset_in_2MB);
            number_of_pages_used++;
        }

        // Check for promotion
        float utilization = static_cast<float>(bitset.count()) / PAGES_IN_2MB_REGION;

        if (utilization > m_threshold_for_promotion) {
            std::get<2>(region_info) = true;
            m_thp_stats.two_mb_promoted++;
            return { base_physical_page, (UInt64)PAGE_SIZE_2MB_BITS };
        }
        else {
            return { base_physical_page + offset_in_2MB, (UInt64)PAGE_SIZE_4KB_BITS };
        }
    }

    // ══════════════════════════════════════════════════════════════════════
    //  STAGE 3: 4KB Revelator Hash
    // ══════════════════════════════════════════════════════════════════════
    std::pair<UInt64, UInt64> tryRevelatorHash(UInt64 address)
    {
        const UInt64 to_hash = address >> PAGE_SIZE_4KB_BITS;

        for (int i = 1; i <= m_number_of_hashes; ++i) {
            UInt64 hash_index = hashFunction(i * to_hash, m_total_pages);

            bool is_free = !m_poisoned_pages[hash_index] &&
                           !m_four_kb_physical_map[hash_index];

            RegionState rs = m_two_mb_physical_map[hash_index / PAGES_IN_2MB_REGION];
            bool is_region_ok = (rs == RegionState::Free ||
                                 rs == RegionState::PoisonedTHP ||
                                 rs == RegionState::ReservedBuddy ||
                                 rs == RegionState::ReservedRevelator4KB);

            if (is_free && is_region_ok) {
                m_hash_stats.successful_data_allocations[i - 1]++;
                m_general_stats.revelator_4kb_data_allocated++;

                m_four_kb_physical_map[hash_index] = 1;
                m_two_mb_physical_map[hash_index / PAGES_IN_2MB_REGION] =
                    RegionState::ReservedRevelator4KB;
                number_of_pages_used++;

                UInt64 phys_target = hash_index + m_kernel_size_in_pages;
                return { phys_target, (UInt64)PAGE_SIZE_4KB_BITS };
            }
        }

        return { (UInt64)-1, (UInt64)0 };
    }

    // ══════════════════════════════════════════════════════════════════════
    //  STAGE 4: Fallback linear scan
    // ══════════════════════════════════════════════════════════════════════
    std::pair<UInt64, UInt64> fallbackScan(UInt64 address)
    {
        m_general_stats.data_allocated_fallback++;
        IntPtr start_idx = hashFunction(address, m_total_pages);
        IntPtr current_idx = start_idx;

        for (UInt64 i = 0; i < m_total_pages; ++i) {
            RegionState rs = m_two_mb_physical_map[current_idx / PAGES_IN_2MB_REGION];

            bool is_free = (m_four_kb_physical_map[current_idx] == 0);
            bool region_ok = (rs == RegionState::Free ||
                              rs == RegionState::PoisonedTHP ||
                              rs == RegionState::ReservedBuddy ||
                              rs == RegionState::ReservedRevelator4KB);

            if (is_free && region_ok) {
                m_four_kb_physical_map[current_idx] = 1;
                m_two_mb_physical_map[current_idx / PAGES_IN_2MB_REGION] =
                    RegionState::ReservedBuddy;
                number_of_pages_used++;

                UInt64 pa = current_idx + m_kernel_size_in_pages;
                return { pa, (UInt64)PAGE_SIZE_4KB_BITS };
            }
            current_idx = (current_idx + 1) % m_total_pages;
        }

        // OOM
        log_file << "[RevelatorTHP] OOM (user): Could not allocate for VA "
                 << address << "\n";
        assert(false);
        return { (UInt64)-1, (UInt64)0 };
    }

    // ── Deallocation ─────────────────────────────────────────────────────
    void deallocate(UInt64 physical_page, UInt64 core_id) override
    {
        m_kernel_physical_map[physical_page] = 0;
        number_of_pages_used--;
    }

    // ── Fragmentation ────────────────────────────────────────────────────
    void fragment_memory(double target_fragmentation) override
    {
        // target_fragmentation: Revelator-level 4KB poisoning fraction
        // THP-level 2MB poisoning comes from Policy
        double target_revelator_frag = 1.0 - target_fragmentation;
        double target_thp_frag = Policy::get_thp_fragmentation_target(m_name);

        int poison_two_mb = (int)(target_thp_frag * (m_total_pages / PAGES_IN_2MB_REGION));
        int revelator_poison_pages = (int)(target_revelator_frag * m_total_pages);

        bool achieved_thp_poisoning = false;
        int poisoned_two_mb_regions = 0;

        // Poison 4KB pages and opportunistically poison 2MB regions
        for (UInt64 i = 0; i < (UInt64)revelator_poison_pages; i++) {
            IntPtr hash_number = 1000;
            while (true) {
                IntPtr hash = hashFunction(i * hash_number, m_total_pages);
                if (!m_poisoned_pages[hash]) {
                    m_poisoned_pages[hash] = true;

                    if (m_two_mb_physical_map[hash / PAGES_IN_2MB_REGION] == RegionState::Free
                        && !achieved_thp_poisoning)
                    {
                        m_two_mb_physical_map[hash / PAGES_IN_2MB_REGION] = RegionState::PoisonedTHP;
                        poisoned_two_mb_regions++;
                        if (poisoned_two_mb_regions >= poison_two_mb)
                            achieved_thp_poisoning = true;
                    }
                    break;
                }
                hash_number++;
            }
            number_of_pages_used++;
        }

        // If we haven't achieved THP poisoning target, poison more 2MB regions
        if (!achieved_thp_poisoning) {
            int iterations = poison_two_mb - poisoned_two_mb_regions;
            for (UInt64 i = 0; i < (UInt64)iterations; i++) {
                IntPtr hash_number = 1000;
                while (true) {
                    IntPtr hash = hashFunction(i * hash_number, m_total_pages);
                    if (m_two_mb_physical_map[hash / PAGES_IN_2MB_REGION] == RegionState::Free) {
                        m_two_mb_physical_map[hash / PAGES_IN_2MB_REGION] = RegionState::PoisonedTHP;
                        poisoned_two_mb_regions++;
                        break;
                    }
                    hash_number++;
                }
                if (poisoned_two_mb_regions >= poison_two_mb)
                    break;
            }
        }
    }

    // ── Demote least-utilized THP region ─────────────────────────────────
    bool demote_page()
    {
        std::vector<std::pair<UInt64, double>> utilization_list;

        for (auto const &[region_id, region_info] : m_two_mb_map) {
            if (std::get<2>(region_info)) continue; // skip promoted
            double util = static_cast<double>(std::get<1>(region_info).count()) / 512.0;
            utilization_list.push_back({region_id, util});
        }

        if (utilization_list.empty()) return false;

        std::sort(utilization_list.begin(), utilization_list.end(),
                  [](const auto &a, const auto &b) { return a.second < b.second; });

        UInt64 region_to_demote_id = utilization_list[0].first;
        auto &region_info = m_two_mb_map[region_to_demote_id];

        UInt64 region_start_pa = std::get<0>(region_info);
        auto &bitset = std::get<1>(region_info);

        // Free unused sub-pages
        UInt64 region_start_idx = region_start_pa - m_kernel_size_in_pages;
        for (UInt64 i = 0; i < 512; ++i) {
            if (!bitset[i])
                m_four_kb_physical_map[region_start_idx + i] = 0;
        }

        // Revert 2MB region state
        m_two_mb_physical_map[region_start_idx / PAGES_IN_2MB_REGION] = RegionState::Free;

        m_thp_stats.two_mb_demoted++;
        m_two_mb_map.erase(region_to_demote_id);
        return true;
    }

    // ── Placeholders / stubs ─────────────────────────────────────────────
    std::vector<Range> allocate_ranges(IntPtr, IntPtr, int) override { return {}; }
    std::vector<Range> allocate_eager_paging(UInt64, UInt64) { return {}; }

    UInt64 givePageFast(UInt64 bytes, UInt64 address = 0, UInt64 core_id = (UInt64)-1) override
    {
        return allocate(bytes, address, core_id, false).first;
    }

    // ── Hash function (CityHash64) ───────────────────────────────────────
    static UInt64 hashFunction(IntPtr address, UInt64 table_size)
    {
        return CityHash64((const char *)&address, 8) % table_size;
    }

private:
    friend Policy;

    // ── Member data ──────────────────────────────────────────────────────
    int    m_max_order;
    int    m_number_of_hashes;
    float  m_threshold_for_promotion;

    UInt64 m_memory_size;
    UInt64 m_kernel_size;
    UInt64 m_total_pages;
    UInt64 m_kernel_size_in_pages;
    UInt64 number_of_pages_used;
    UInt64 faults;

    // Kernel page pool
    std::vector<int> m_kernel_physical_map;

    // Data page pool
    std::vector<int> m_four_kb_physical_map;

    // 2MB region state
    std::vector<RegionState> m_two_mb_physical_map;

    // Fragmentation
    std::vector<bool> m_poisoned_pages;
    std::vector<bool> m_poisoned_pages_kernel;

    // Virtual-2MB-region → (physical_start_page, 4KB usage bitset, is_promoted)
    std::map<UInt64, std::tuple<UInt64, std::bitset<PAGES_IN_2MB_REGION>, bool>> m_two_mb_map;

    // Stats
    HashStats    m_hash_stats;
    GeneralStats m_general_stats;
    THPStats     m_thp_stats;

    // Logging
    std::ofstream log_file;
    std::string   log_file_name;
    std::ofstream *log_stream = nullptr;
};
