// #pragma once
// #include "physical_memory_allocator.h"
// #include "rangelb.h"
// #include <vector>
// #include <map>
// #include <bitset>
// #include <string>
// #include <semaphore.h>
// #include <unordered_map>
// #include<fixed_types.h>

// using namespace std;
// class RevelatorAllocator : public PhysicalMemoryAllocator
// {
// public:
//     RevelatorAllocator(String name, int max_order, int kernel_size, string frag_type, int number_of_hashes);
//     void init();
//     UInt64 allocate(UInt64 size, IntPtr address = 0, UInt64 core_id = -1, bool is_pagetable_allocation = false);
//     void deallocate(UInt64, UInt64 core_id);
//     std::vector<Range> allocate_eager_paging(UInt64, UInt64 core_id);
//     void print_allocator(UInt64 core_id);
//     UInt64 hashFunction(IntPtr address, int table_size);
    
//     std::unordered_map<UInt64, std::vector<double>> allocation_hash_stats;   // Key: PID or core and Value is a vector of stats for the N available hash functions
//     std::unordered_map<UInt64, UInt64> total_allocations;  
//     void perform_init_random(double target_fragmentation, double target_memory_percent, bool store_in_file = false, UInt64 core_id = -1);
//     struct
//     {
//         std::vector<float> *fragmentation;
//         std::vector<float> *two_mb_util_ratio;
//         UInt64 four_kb_allocated;
//         UInt64 two_mb_reserved;
//         UInt64 two_mb_promoted;
//         UInt64 two_mb_demoted;

//         /*ianganz*/ UInt64 times_added_memory;
//     } stats;

//     void print_allocation_stats(UInt64 core_id);

//     UInt64 m_frag_factor;
//     int m_kernel_size;


// protected:
//     int m_max_order;
//     UInt64 m_total_pages;
//     std::vector<std::tuple<UInt64, UInt64>> memory_allocations; // address, core_id
//     int number_of_hashes;
    
// };
