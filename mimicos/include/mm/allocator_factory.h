#pragma once


// mimicos/src/
#include "physical_allocator/policies/buddy_policy.h"           
#include "physical_allocator/policies/reserve_thp_policy.h"           
#include "physical_allocator/policies/baseline_allocator_policy.h"

//We include the relevant headers from sniper/include/ - we added the core 
// implementations of the allocators there to be accessible from both MimicOS and Sniper

#include "memory_management/physical_memory_allocators/reserve_thp.h" 
#include "memory_management/physical_memory_allocators/baseline.h"

// C++ libs
#include <string>
#include <iostream>

using VirtuosoBaselineAllocator = BaselineAllocator<Virtuoso::Baseline::NoMetricsPolicy>;
using VirtuosoTHPAllocator      = ReservationTHPAllocator<Virtuoso::ReserveTHP::NoMetricsPolicy>;

class AllocatorFactory
{
public:
    static PhysicalMemoryAllocator *createAllocator(String allocator_name, int memory_size, int max_order, int kernel_size, String frag_type, int threshold_for_promotion = -1)
    {
        
        std::cout << "[MimicOS] [createAllocator] Creating allocator: " << allocator_name << std::endl;

        if (allocator_name == "baseline")
        {
            std::cout << "[MimicOS] [createAllocator] Created VirtuosoBaselineAllocator" << std::endl;
            return new VirtuosoBaselineAllocator(allocator_name, memory_size, max_order, kernel_size, frag_type);
        }
        else if (allocator_name == "reserve_thp")
        {
            std::cout << "[MimicOS] [createAllocator] Created VirtuosoTHPAllocator" << std::endl;
            return new VirtuosoTHPAllocator(allocator_name, memory_size, max_order, kernel_size, frag_type, threshold_for_promotion);
        }
        else
        {
            std::cout << "[MimicOS/AllocatorFactory] allocator_name = " << allocator_name <<
                         " was not migrated yet ..." << std::endl;
            std::cout << "[MimicOS/AllocatorFactory] allocator_name.size() = " << allocator_name.size() << std::endl;
            return nullptr;
        }
        
    }
};