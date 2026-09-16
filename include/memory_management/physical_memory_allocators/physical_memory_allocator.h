#pragma once

#include "fixed_types.h"
#include <vector>
#include "../misc/vma.h"

class PhysicalMemoryAllocator
{
public:
    PhysicalMemoryAllocator(String name, UInt64 memory_size, UInt64 kernel_size) : m_name(name),
                                                                                   m_memory_size(memory_size),
                                                                                   m_kernel_size(kernel_size)
    {
        kernel_start_address = 4096; // First 4KiB is reserved for root->emulated_ppn page
        kernel_end_address = kernel_size * 1024 * 1024;
        
        // Reserve 100MB for instruction pages within the kernel region
        // Instruction pages start after regular kernel allocation area
        instruction_reserved_size = 100 * 1024 * 1024; // 100MB in bytes
        instruction_start_address = kernel_end_address; // Instructions start right after kernel
        instruction_current_address = instruction_start_address;
        instruction_end_address = instruction_start_address + instruction_reserved_size;
    };

    virtual ~PhysicalMemoryAllocator() {};
    // Returns <physical page, page_size>
    // is_instruction_allocation: if true, allocate from instruction reserved area
    virtual std::pair<UInt64,UInt64> allocate(UInt64 size, UInt64 address = 0, UInt64 core_id = -1, bool is_pagetable_allocation = false, bool is_instruction_allocation = false) = 0;
    virtual std::vector<Range> allocate_ranges(IntPtr start_va, IntPtr end_va, int app_id) = 0;
    virtual void deallocate(UInt64 address, UInt64 core_id) = 0;
    virtual void fragment_memory(double target_fragmentation) = 0;
    
    /**
     * fragment_memory_to_count(UInt64 target_free_2mb_pages):
     *   Fragment memory until exactly the specified number of free 2MB pages remain.
     *   Default implementation falls back to ratio-based fragmentation.
     *   Allocators that support count-based fragmentation should override this.
     *
     * @param target_free_2mb_pages The exact number of free 2MB pages to maintain
     */
    virtual void fragment_memory_to_count(UInt64 target_free_2mb_pages)
    {
        // Default implementation: convert count to approximate ratio and use ratio-based
        // Subclasses (like ReservationTHPAllocator) override this with proper implementation
        // This fallback assumes roughly 50% of memory could be 2MB pages
        double approx_ratio = (target_free_2mb_pages > 0) ? 0.5 : 0.0;
        fragment_memory(approx_ratio);
    }
    
    virtual float getAllocRatioForHash(int h) {return h; /* Override in derived allocators */};
    virtual UInt64 givePageFast(UInt64 bytes, UInt64 address = 0, UInt64 core_id = -1) = 0;
    
    virtual UInt64 handle_page_table_allocations(UInt64 bytes, UInt64 core_id = (UInt64)-1)
    {
        (void)core_id; // Non-NUMA allocators ignore core_id
        UInt64 temp = kernel_start_address;
        kernel_start_address += bytes; // Allocate contiguous frames for page table structures
        return temp/4096;
    }

    virtual void handle_page_table_deallocations(UInt64 bytes)
    {
        kernel_start_address -= bytes;
    }


    virtual UInt64 handle_fine_grained_page_table_allocations(UInt64 bytes)
    {
        UInt64 temp = kernel_start_address;
        kernel_start_address += bytes; // Allocate contiguous frames for page table structures
        return temp;
    }
    String getName() { return m_name; }
    
    // Allocate from reserved instruction area
    virtual std::pair<UInt64, UInt64> allocateInstruction(UInt64 size)
    {
        if (instruction_current_address + size > instruction_end_address) {
            // Out of instruction reserved space - fall back to error
            return std::make_pair((UInt64)-1, 12);
        }
        UInt64 page = instruction_current_address / 4096;
        instruction_current_address += size;
        return std::make_pair(page, 12);
    }
    
    /**
     * @brief Get the NUMA node ID for a given physical page number.
     * 
     * Returns 0 for non-NUMA allocators (single implicit node).
     * NUMA-aware allocators (e.g., NumaReservationTHPAllocator) override this
     * to query their internal buddy allocator for the authoritative PPN→node mapping.
     *
     * @param ppn Physical page number (4KB granularity)
     * @return NUMA node ID (0-based)
     */
    virtual UInt32 getNumaNodeForPPN(UInt64 ppn) const { return 0; }

    /**
     * @brief Dump any final statistics before simulation ends
     * 
     * Called from MimicOS destructor to ensure allocator stats are written
     * while the output directory is still valid.
     * Default implementation does nothing - subclasses override as needed.
     */
    virtual void dumpFinalStats() {}
    
protected:
    String m_name;
    UInt64 m_memory_size; // in mbytes
    UInt64 kernel_start_address;
    UInt64 kernel_end_address;
    UInt64 m_kernel_size; // in mbytes
    
    // Instruction reserved area (100MB)
    UInt64 instruction_reserved_size;
    UInt64 instruction_start_address;
    UInt64 instruction_current_address;
    UInt64 instruction_end_address;

    /* Which path the last allocate() took, for allocators that distinguish a
     * per-CPU pool hit from a slow-path refill:
     *   0 = slow path (buddy refill / zone lock)
     *   1 = fast path (per-CPU pool hit)
     *   2 = the allocator does not distinguish
     *
     * Read by the userspace MimicOS fault profile, which learns a separate mean
     * cost per class.  Only linux_buddy_anon sets it today; everything else
     * leaves it at 2, which is why it is a plain field with a default rather
     * than a virtual.
     */
    uint8_t last_alloc_fastpath = 2;

};
