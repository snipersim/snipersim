#pragma once
#include <vector>
#include <map>
#include <bitset>

#include "buddy_allocator.h"
#include "physical_memory_allocator.h"
#include "vma.h"


using namespace std;

class ReservationTHPAllocator : public PhysicalMemoryAllocator
{
public:
    ReservationTHPAllocator(String name, int memory_size, int max_order, int kernel_size, String frag_type, float threshold_for_promotion);
    ~ReservationTHPAllocator();

    std::pair<UInt64, UInt64> allocate(UInt64 size, UInt64 address = 0, UInt64 core_id = -1, bool is_pagetable_allocation = false);
    std::vector<Range> allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id);
    UInt64 givePageFast(UInt64 size, UInt64 address = 0, UInt64 core_id = -1);
    void deallocate(UInt64 region, UInt64 core_id);
    void fragment_memory();
    std::pair<UInt64,bool> checkFor2MBAllocation(UInt64 address, UInt64 core_id);
    bool demote_page();

    IntPtr isLargePageReserved(IntPtr address){
        if (two_mb_map.find(address >> 21) != two_mb_map.end())
            return get<0>(two_mb_map[address >> 21]);
        return -1;
    }

    virtual UInt64 handle_page_table_allocations(UInt64 bytes)
    {
        stats.kernel_pages_used += 1;
        UInt64 temp = kernel_start_address;
        kernel_start_address += bytes; // This function is useful mainly for the page table allocation in the HDC, HT and Elastic Cuckoo Hash Table; For Radix we can invoke the conventional allocator directly
       // std::cout << "Kernel pages allocated" << stats.kernel_pages_used << std::endl;

        return temp/4096;
    }

    virtual void handle_page_table_deallocations(UInt64 bytes)
    {
        stats.kernel_pages_used -= 1;
        kernel_start_address -= bytes;
        //std::cout << "Kernel pages after deallocation: " << stats.kernel_pages_used << std::endl;

    }

    UInt64 getFreePages() const;
    UInt64 getTotalPages() const;
    double getAverageSizeRatio();
    double getLargePageRatio();

    struct
    {
        UInt64 four_kb_allocated;
        UInt64 two_mb_reserved;
        UInt64 two_mb_promoted;
        UInt64 two_mb_demoted;
        UInt64 total_allocations;
        UInt64 kernel_pages_used;
    } stats;

    double (ReservationTHPAllocator::*frag_fun)();

    std::ofstream log_file;
    std::string log_file_name;

protected:
    Buddy *buddy_allocator;
    // This threshold is used to determine when to promote a 2MB region based on the 4KB page utilization
    float threshold_for_promotion;
    // This map is used to track 2MB-large regions
    std::map<UInt64, std::tuple<UInt64, std::bitset<512>, bool>> two_mb_map; // <region_2MB, <region_begin, bitset, promoted>>
    UInt64 m_frag_factor;

};
