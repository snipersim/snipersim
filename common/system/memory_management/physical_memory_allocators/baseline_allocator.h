#pragma once
#include "physical_memory_allocator.h"
#include <vector>
#include <map>
#include <bitset>
#include "buddy_allocator.h"
#include "vma.h"

using namespace std;

class BaselineAllocator : public PhysicalMemoryAllocator
{
public:
    BaselineAllocator(String name, int memory_size, int max_order, int kernel_size, String frag_type);
    ~BaselineAllocator(){};

    std::pair<UInt64,UInt64> allocate(UInt64 size, UInt64 address = 0, UInt64 core_id = -1, bool is_pagetable_allocation = false);
    UInt64 givePageFast(UInt64 size, UInt64 address = 0, UInt64 core_id = -1);
    std::vector<Range> allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id);
    void deallocate(UInt64 address, UInt64 core_id);
    bool demote_page();

    void calculate_order(UInt64 size);
    void print_allocator(){};
    UInt64 getFreePages() const;
    UInt64 getTotalPages() const;
    double getAverageSizeRatio();
    double getLargePageRatio();

    void fragment_memory();

    struct
    {
    } stats;

    std::ofstream log_file;
    std::string log_file_name;

    double (BaselineAllocator::*frag_fun)();

protected:

    Buddy *buddy_allocator;
    std::map<UInt64, UInt64> allocated_map;
    int m_kernel_size;
    int m_memory_size;
    int m_max_order;

};
