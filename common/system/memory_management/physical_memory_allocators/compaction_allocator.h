// #pragma once
// #include "physical_memory_allocator.h"
// #include <vector>
// #include <map>
// #include <bitset>
// #include <unordered_map>

// #include "register_metrics.h"
// #include "buddy.h"

// using namespace std;

// class CompactionTHPAllocator : public PhysicalMemoryAllocator
// {
// public:
//     CompactionTHPAllocator(int memory_size, int max_order, int kernel_size, string frag_type, double threshold_for_promotion);
//     ~CompactionTHPAllocator();



//     bool demote_page();
//     UInt64 allocate(UInt64 size, UInt64 address = 0, UInt64 core_id = -1);
//     UInt64 givePageFast(UInt64 size, UInt64 address = 0, UInt64 core_id = -1);
//     void deallocate(UInt64 region);




//     UInt64 getFreePages() const;
//     UInt64 getTotalPages() const; 
//     double getAverageSizeRatio();
//     double getLargePageRatio();

//     void fragmentMemory(double fragmentation) override;

//     struct
//     {
//         std::vector<float> *fragmentation;
//         std::vector<float> *two_mb_util_ratio;
//         UInt64 four_kb_allocated;
//         UInt64 two_mb_reserved;
//         UInt64 two_mb_promoted;
//         UInt64 two_mb_demoted;
//     } stats;


//     double (CompactionTHPAllocator::*frag_fun)();

// protected:

//     Buddy *buddy_allocator;
//     // This threshold is used to determine when to promote a 2MB region based on the 4KB page utilization
//     double threshold_for_promotion;
//     // This map is used to track 2MB-large regions 

//     struct RegionInfo {
//         bool isPromoted;
//         std::vector<UInt64> physicalFrameNumber; // Vector to store physical addresses of 4KB pages within the 2MB region.
//     };

//     std::unordered_map<UInt64, RegionInfo> two_mb_map;  // Map to track 2MB re

//     UInt64 m_frag_factor;

// };
