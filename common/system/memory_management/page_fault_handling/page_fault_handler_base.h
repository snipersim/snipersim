
#pragma once
#include "physical_memory_allocator.h"
#include <iostream>
#include <fstream>

using namespace std;
class PageFaultHandlerBase
{
    protected:
        PhysicalMemoryAllocator *allocator;

    public:

        PageFaultHandlerBase(PhysicalMemoryAllocator *allocator){
            this->allocator = allocator;
        }
        
        ~PageFaultHandlerBase(){};

       virtual void allocatePagetableFrames(UInt64 address, UInt64 core_id, UInt64 ppn, int page_size, int frame_number) = 0;
       virtual void handlePageFault(UInt64 address, UInt64 core_id, int frames) = 0;
};