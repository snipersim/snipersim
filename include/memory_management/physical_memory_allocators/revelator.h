#pragma once

#include "physical_memory_allocator.h"
#include <vector>
#include <fstream>
#include <city.h>

// forward declare policy concept struct later
template<typename Policy>
class RevelatorAllocator : public PhysicalMemoryAllocator, private Policy
{
public:
    struct HashStats {
        UInt64 *hits;
        UInt64 *tries;
        UInt64 *swap_attempts;
        UInt64 *swap_attempts_same_address;
        UInt64 *successful_pt_allocations;
    };

    struct AllocationEntry {
        IntPtr address;
        bool   is_free;
        bool   is_poisoned;
        bool   is_pagetable_allocation;
        UInt64 app_id;
    };


    RevelatorAllocator(String name,
                       int max_order,
                       UInt64 kernel_size,
                       String frag_type,
                       int m_number_of_hashes,
                       UInt64 memory_size,
                       double _target_frag,
                       bool _enable_aggressive_swapouts,
                       int _infrequency_threshold,
                       double hash1_usage_threshold)
        : PhysicalMemoryAllocator(name, memory_size, kernel_size)
        , m_max_order(max_order)
        , enable_aggressive_swapouts(_enable_aggressive_swapouts)
        , infrequency_threshold(_infrequency_threshold)
        , hash1_usage_threshold(hash1_usage_threshold)
    {

        m_swap_mode_enabled  = false;
        number_of_hashes     = m_number_of_hashes;
        target_frag          = _target_frag;
        m_memory_size        = memory_size;
        m_kernel_size        = kernel_size;
        number_of_pages_used = 0;

        m_total_pages = m_memory_size * 1024 / 4 - m_kernel_size * 1024 / 4;

        memory_allocations.assign(m_total_pages,
            AllocationEntry{(IntPtr)-1, true, false, false, (UInt64)-1});

        // allocate stats arrays
        hash_stats.hits                      = (UInt64*) malloc(sizeof(UInt64) * number_of_hashes);
        hash_stats.tries                     = (UInt64*) malloc(sizeof(UInt64) * number_of_hashes);
        hash_stats.swap_attempts             = (UInt64*) malloc(sizeof(UInt64) * number_of_hashes);
        hash_stats.swap_attempts_same_address = (UInt64*) malloc(sizeof(UInt64) * number_of_hashes);
        hash_stats.successful_pt_allocations = (UInt64*) malloc(sizeof(UInt64) * number_of_hashes);

        for (int i = 0; i < number_of_hashes; i++) {
            hash_stats.hits[i]                      = 0;
            hash_stats.tries[i]                     = 0;
            hash_stats.swap_attempts[i]             = 0;
            hash_stats.swap_attempts_same_address[i]= 0;
            hash_stats.successful_pt_allocations[i] = 0;
        }

        // --- NEW: let the Policy decide how to register stats
        Policy::register_stats(name, *this);

        // policy-specific init hook (like in ReservationTHPAllocator)
        Policy::on_init(name,
                        memory_size,
                        kernel_size,
                        m_max_order,
                        frag_type,
                        this);
    }

    int get_number_of_hashes() const { return number_of_hashes; }
    
    void init()
    {
        UInt64 total_mem_in_pages = m_memory_size * 1024 / 4 - m_kernel_size * 1024 / 4;
        m_total_pages = total_mem_in_pages;
        memory_allocations.assign(total_mem_in_pages,
            AllocationEntry{(IntPtr)-1, true, false, false, (UInt64)-1});
    }

    std::pair<UInt64, UInt64> allocate(UInt64 size,
                                       IntPtr address,
                                       UInt64 app_id,
                                       bool is_pagetable_allocation,
                                       bool is_instruction_allocation = false) override
    {
        // Instruction allocations go to the reserved instruction area
        if (is_instruction_allocation)
        {
            return this->allocateInstruction(size);
        }

        [[maybe_unused]] int size_in_4kb_pages = 1;
        [[maybe_unused]] UInt64 offset   = address & 0xFFF;
        UInt64 to_hash  = (is_pagetable_allocation) ? address >> 21 : address >> 12;

        // === policy hook: maybe enable swap mode based on stats ===
        Policy::maybe_enable_swap_mode(*this);

        IntPtr hash = 0;

        for (int i = 1; i <= number_of_hashes; i++)
        {
            hash_stats.tries[i-1]++;
            hash = hashFunction(i * to_hash, m_total_pages);
            auto &entry = memory_allocations[hash];

            if (entry.is_free && !entry.is_poisoned)
            {
                entry.address = address;
                entry.is_free = false;
                entry.app_id  = app_id;
                entry.is_pagetable_allocation = is_pagetable_allocation;

                UInt64 page_address = (hash + (m_kernel_size * 1024 / 4));
                number_of_pages_used++;
                hash_stats.hits[i-1]++;

                if (is_pagetable_allocation)
                    hash_stats.successful_pt_allocations[i-1]++;

                return std::make_pair(page_address, 12);
            }
            // === policy-driven swapping (MimicOS or “no-op” depending on Policy) ===
            else if (m_swap_mode_enabled &&
                     Policy::should_attempt_swap(*this, i, is_pagetable_allocation, entry))
            {
                bool swapped =
                    Policy::try_swap_out_victim(*this,
                                                hash,
                                                address,
                                                app_id,
                                                entry,
                                                i-1 /*hash index for stats*/);

                if (swapped)
                {
                    // Now the slot should be free for our new allocation
                    auto &slot = memory_allocations[hash];
                    slot.address = address;
                    slot.is_free = false;
                    slot.app_id  = app_id;
                    slot.is_pagetable_allocation = is_pagetable_allocation;

                    UInt64 page_address = (hash + (m_kernel_size * 1024 / 4));
                    number_of_pages_used++;
                    return std::make_pair(page_address, 12);
                }
            }
        }

        // === fallback linear scan ===
        IntPtr index = hash;
        for (UInt64 i = 0; i < memory_allocations.size(); i++)
        {
            index = (index + 1) % memory_allocations.size();
            auto &entry = memory_allocations[index];

            if (entry.is_free || entry.is_poisoned)
            {
                entry.address = address;
                entry.is_free = false;
                entry.app_id  = app_id;
                entry.is_pagetable_allocation = is_pagetable_allocation;

                UInt64 page_address = (index + (m_kernel_size * 1024 / 4));
                number_of_pages_used++;
                return std::make_pair(page_address, 12);
            }
        }

        log_file << "[VirtuOS:RevelatorAllocator] Not enough space in memory." << std::endl;
        exit(1);
    }

    void deallocate(UInt64 index, UInt64 core_id) override
    {
        index = index - (m_kernel_size * 1024 / 4);
        auto &entry = memory_allocations[index];
        entry.is_free = true;
        entry.app_id  = (UInt64)-1;
        entry.is_pagetable_allocation = false;
        entry.address = (IntPtr)-1;
        number_of_pages_used--;
    }

    std::vector<Range> allocate_ranges(IntPtr, IntPtr, int) override {
        return {};
    }

    std::vector<Range> allocate_eager_paging(UInt64, UInt64) {
        return {};
    }

    void fragment_memory(double target_fragmentation) override {
        perform_init_random(target_fragmentation);
    }

    static UInt64 hashFunction(IntPtr address, int table_size)
    {
        return CityHash64((const char *)&address, 8) % table_size;
    }

    // Expose some things to policies
    HashStats& get_hash_stats() { return hash_stats; }
    const HashStats& get_hash_stats() const { return hash_stats; }

    float getAllocRatioForHash(int h) override
    {
        if (hash_stats.tries[h] == 0) return 0.0f;
        return (float)(hash_stats.hits[h]) / (float)(hash_stats.tries[h]);
    }

    UInt64 givePageFast(UInt64 bytes, UInt64 address = 0, UInt64 core_id = (UInt64)-1) override
    {
        // Fast path: delegate to the main allocator logic and return the physical page.
        return allocate(bytes, address, core_id, false).first;
    }

    struct VictimInfo {
        IntPtr address;
        UInt64 app_id;
    };

    VictimInfo get_victim_info(size_t idx) const {
        const auto &e = memory_allocations[idx];
        return VictimInfo{ e.address, e.app_id };
    }

    bool is_swap_mode_enabled() const { return m_swap_mode_enabled; }
    void enable_swap_mode() { m_swap_mode_enabled = true; }

    int  get_infrequency_threshold() const { return infrequency_threshold; }
    bool aggressive_swapouts_enabled() const { return enable_aggressive_swapouts; }

private:

    friend Policy; // so Policy can access private members if needed

    int m_max_order;
    bool enable_aggressive_swapouts;
    int  infrequency_threshold;
    double hash1_usage_threshold;
    double target_frag;

    UInt64 m_memory_size;
    UInt64 m_kernel_size;
    UInt64 m_total_pages;
    UInt64 number_of_pages_used = 0;

    int number_of_hashes;
    bool m_swap_mode_enabled;

    std::vector<AllocationEntry> memory_allocations;

    HashStats hash_stats;
    std::ofstream log_file;
    std::string  log_file_name;
    std::ofstream *log_stream = nullptr;

    void perform_init_random(double target_memory_percent)
    {
        double target_mem = 1.0f - target_memory_percent;
        UInt64 pages = (UInt64)(m_total_pages * target_mem);

        for (UInt64 i = 0; i < pages; i++)
        {
            IntPtr hash_number = 1000;
            while (true)
            {
                IntPtr hash = hashFunction(i * hash_number, m_total_pages);
                if (!memory_allocations[hash].is_poisoned)
                {
                    memory_allocations[hash] =
                        AllocationEntry{(IntPtr)-1, true, true, false, (UInt64)-1};
                    break;
                }
                hash_number++;
            }
            number_of_pages_used++;
        }
    }
};
