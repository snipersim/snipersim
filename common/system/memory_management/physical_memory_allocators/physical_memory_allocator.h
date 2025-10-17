#pragma once

#include "fixed_types.h"
#include <vector>
#include "semaphore.h"
#include "vma.h"

class PhysicalMemoryAllocator
{
public:
    PhysicalMemoryAllocator(String name, UInt64 memory_size, UInt64 kernel_size) : m_name(name),
                                                                                   m_memory_size(memory_size),
                                                                                   m_kernel_size(kernel_size)
    {
        kernel_start_address = 0;
        kernel_end_address = kernel_size * 1024 * 1024;
    };

    virtual ~PhysicalMemoryAllocator() {};
    // Returns <physical page, page_size>
    virtual std::pair<UInt64,UInt64> allocate(UInt64 size, UInt64 address = 0, UInt64 core_id = -1, bool is_pagetable_allocation = false) = 0;
    virtual std::vector<Range> allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id) = 0;
    virtual void deallocate(UInt64 address, UInt64 core_id) = 0;
    virtual void fragment_memory() = 0;

    virtual UInt64 handle_page_table_allocations(UInt64 bytes)
    {
        UInt64 temp = kernel_start_address;
        kernel_start_address += bytes; // This function is useful mainly for the page table allocation in the HDC, HT and Elastic Cuckoo Hash Table; For Radix we can invoke the conventional allocator directly
        return temp/4096;
    }

    virtual void handle_page_table_deallocations(UInt64 bytes)
    {
        kernel_start_address -= bytes;
    }


    virtual UInt64 handle_fine_grained_page_table_allocations(UInt64 bytes)
    {
        UInt64 temp = kernel_start_address;
        kernel_start_address += bytes; // This function is useful mainly for the page table allocation in the HDC, HT and Elastic Cuckoo Hash Table; For Radix we can invoke the conventional allocator directly
        return temp;
    }
    String getName() { return m_name; }

    UInt64 getKernelStartAddress() { return kernel_start_address; }
    
protected:
    String m_name;
    UInt64 m_memory_size; // in mbytes
    UInt64 kernel_start_address;
    UInt64 kernel_end_address;
    UInt64 m_kernel_size; // in mbytes

    
};
