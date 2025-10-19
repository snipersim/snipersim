#pragma once
#include <vector>
#include <map>
#include <bitset>
#include "buddy_allocator.h"
#include "vma.h"
#include "physical_memory_allocator.h"

using namespace std;

class EagerPagingAllocator : public PhysicalMemoryAllocator
{
public:
    EagerPagingAllocator(String name, int memory_size, int max_order, int kernel_size, String frag_type);
    ~EagerPagingAllocator();

    std::pair<UInt64,UInt64> allocate(UInt64 size, UInt64 address = 0, UInt64 core_id = -1, bool is_pagetable_allocation = false);
    std::vector<Range> allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id);
    void deallocate(UInt64 address, UInt64 core_id);
    void fragment_memory();
    UInt64 givePageFast(UInt64 bytes, UInt64 address, UInt64 core_id);

    UInt64 getFreePages() const;
    UInt64 getTotalPages() const;
    double getAverageSizeRatio();
    double getLargePageRatio();

    void fragmentMemory(double fragmentation);

    struct
    {
        UInt64 physical_ranges_per_vma;
        UInt64 deviation_of_physical_ranges;
        UInt64 total_allocated_vmas;
        UInt64 total_allocated_physical_ranges;
    } stats;

    double (EagerPagingAllocator::*frag_fun)();

protected:
    Buddy *buddy_allocator;
    UInt64 m_frag_factor;

    std::ofstream log_file;
    std::string log_file_name;
};
