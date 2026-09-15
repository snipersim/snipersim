#pragma once

#include "physical_memory_allocator.h"
#include "revelator_types.h"
#include <vector>
#include <fstream>
#include <cassert>
#include <city.h>

/**
 * @class RevelatorSimpleAllocator
 * @brief Template-based hash allocator using a 2-tier data allocation strategy:
 *
 *   1. **4KB Revelator Hash** – Hash virtual page address (>> 12) into 4KB
 *      physical space; succeed only if the backing 2MB region state allows it
 *      (Free, PoisonedTHP, ReservedBuddy, or ReservedRevelator4KB).
 *   2. **Fallback linear scan** – Starting from hash(address), scan linearly
 *      for any free 4KB page in an allowed region.
 *
 * Kernel / page-table pages follow a separate 2-stage path:
 *   K1. Hash-based placement in the kernel pool (hash address >> 21).
 *   K2. Linear-scan fallback over the kernel pool.
 *
 * This is a simplified version of RevelatorTHPAllocator that omits the
 * 2MB Revelator hash (Stage 1) and THP reservation/promotion (Stage 2).
 *
 * Template parameter Policy provides Sniper-specific hooks:
 *   - register_stats(name, alloc)
 *   - on_init(name, memory_size, kernel_size, max_order, frag_type, alloc_ptr)
 */
template<typename Policy>
class RevelatorSimpleAllocator : public PhysicalMemoryAllocator, private Policy
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
    };

    // ── Constructor ──────────────────────────────────────────────────────
    RevelatorSimpleAllocator(String name,
                             UInt64 memory_size,
                             int    max_order,
                             UInt64 kernel_size,
                             String frag_type,
                             int    number_of_hashes)
        : PhysicalMemoryAllocator(name, memory_size, kernel_size)
        , m_max_order(max_order)
        , m_number_of_hashes(number_of_hashes)
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
        m_general_stats = {0, 0, 0, 0, 0};

        // --- Policy hooks ---
        Policy::register_stats(name, *this);
        Policy::on_init(name, memory_size, kernel_size, max_order, frag_type, this);
    }

    ~RevelatorSimpleAllocator()
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

        // --- Kernel / page-table allocation ---
        if (is_pagetable_allocation) {
            m_general_stats.total_page_table_allocations++;
            return allocateKernelPage(address);
        }

        m_general_stats.total_data_allocations++;

        // ── STAGE 1: 4KB Revelator hash ─────────────────────────────────
        auto revelator_4kb_result = tryRevelatorHash(address);
        if (revelator_4kb_result.first != (UInt64)-1) {
            m_hash_stats.total_hash_data_allocations++;
            return revelator_4kb_result;
        }

        // ── STAGE 2: Fallback linear scan ────────────────────────────────
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
        log_file << "[RevelatorSimple] OOM (kernel): Could not allocate for VA "
                 << address << "\n";
        assert(false);
        return { (UInt64)-1, (UInt64)PAGE_SIZE_4KB_BITS };
    }

    // ══════════════════════════════════════════════════════════════════════
    //  STAGE 1: 4KB Revelator Hash
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
    //  STAGE 2: Fallback linear scan
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
        log_file << "[RevelatorSimple] OOM (user): Could not allocate for VA "
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
        double target_frag = 1.0 - target_fragmentation;
        UInt64 poison_pages = (UInt64)(target_frag * m_total_pages);

        for (UInt64 i = 0; i < poison_pages; i++) {
            IntPtr hash_number = 1000;
            while (true) {
                IntPtr hash = hashFunction(i * hash_number, m_total_pages);
                if (!m_poisoned_pages[hash]) {
                    m_poisoned_pages[hash] = true;
                    break;
                }
                hash_number++;
            }
            number_of_pages_used++;
        }
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

    // Stats
    HashStats    m_hash_stats;
    GeneralStats m_general_stats;

    // Logging
    std::ofstream log_file;
    std::string   log_file_name;
    std::ofstream *log_stream = nullptr;
};
