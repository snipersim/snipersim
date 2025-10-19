#pragma once
#include "baseline_allocator.h"
#include "reserve_thp.h"
#include "simulator.h"
#include "config.hpp"


class AllocatorFactory
{
public:
    static PhysicalMemoryAllocator* createAllocator(String mimicos_name){

        String allocator_name = Sim()->getCfg()->getString("perf_model/"+mimicos_name+"/memory_allocator_name");
        String allocator_type = Sim()->getCfg()->getString("perf_model/"+mimicos_name+"/memory_allocator_type");

        int memory_size = Sim()->getCfg()->getInt("perf_model/"+allocator_name+"/memory_size");
        int kernel_size = Sim()->getCfg()->getInt("perf_model/"+allocator_name+"/kernel_size");

        std::cout << "[MimicOS] Kernel size in MB: " << kernel_size << std::endl;

        if (allocator_type == "baseline"){

            String frag_type = Sim()->getCfg()->getString("perf_model/"+allocator_name+"/frag_type");
            int max_order = Sim()->getCfg()->getInt("perf_model/"+allocator_name+"/max_order");
            return new BaselineAllocator(allocator_name, memory_size, max_order, kernel_size, frag_type);
        
        }
        else if (allocator_type == "reserve_thp"){ // Based on FreeBSD's reservation-based THP allocator

            String frag_type = Sim()->getCfg()->getString("perf_model/"+allocator_name+"/frag_type");
            int max_order = Sim()->getCfg()->getInt("perf_model/"+allocator_name+"/max_order");
            float threshold_for_promotion = Sim()->getCfg()->getFloat("perf_model/"+allocator_name+"/threshold_for_promotion");
            return new ReservationTHPAllocator(allocator_name, memory_size, max_order,kernel_size,frag_type, threshold_for_promotion);

        }
        
        else{
            std::cout << "[Sniper] Allocator not found" << std::endl;
            return nullptr;
        }
    }
};