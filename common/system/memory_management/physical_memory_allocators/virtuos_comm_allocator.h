// #pragma once
// #include "physical_memory_allocator.h"
// #include "semaphore.h"
// #include "virtuos.h"

// class VirtuosCommModule : public PhysicalMemoryAllocator
// {
// public:
//     struct
//     {
//         UInt64 num_page_faults;
//         UInt64 page_fault_latency_sum;
//     } stats;
//     bool memory_mapped_not_pipes;
//     int counter;
//     VirtuosCommModule(String name, int max_order);
//     UInt64 allocate(UInt64 size, UInt64 address = 0, UInt64 core_id = 0, bool is_pagetable_allocation = false);
//     void deallocate(UInt64, UInt64 core_id);
//     std::vector<Range> allocate_eager_paging(UInt64, UInt64 core_id);
//     void perform_init_random(double target_fragmentation, double target_memory_percent, bool store_in_file = false, UInt64 core_id = -1);
//     void print_allocator(UInt64 core_id);
// };
