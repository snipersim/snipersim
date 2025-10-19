
#pragma once

#include "buddy_allocator.h"
#include "physical_memory_allocator.h"
#include "page_fault_handler.h"
#include "pagetable.h"
#include "subsecond_time.h"
#include <unordered_map>

using namespace std;

class MimicOS
{

private:

    String mimicos_name;
    PageFaultHandlerBase *page_fault_handler;
    ComponentLatency m_page_fault_latency;

    PhysicalMemoryAllocator *m_memory_allocator; // This is the physical memory allocator

    

    String page_table_type;
    String page_table_name;

    String range_table_type;
    String range_table_name;
    
    int number_of_page_sizes;
    int *page_size_list;

    // Use an unordered map to store the page table for each application
    std::unordered_map<UInt64, ParametricDramDirectoryMSI::PageTable*> page_tables;

    // Use an unordered map to store the virtual memory areas for each application
    bool vmas_provided;
    std::unordered_map<UInt64, std::vector<VMA>> vm_areas;

public:
    MimicOS();
    ~MimicOS();

    void handle_page_fault(IntPtr address, IntPtr core_id, int frames);
    void createApplication(int app_id);

    String getName() { return mimicos_name; }

    PhysicalMemoryAllocator *getMemoryAllocator() { return m_memory_allocator; }

    ParametricDramDirectoryMSI::PageTable* getPageTable(int app_id) { return page_tables[app_id]; }

    std::vector<VMA> getVMA(int app_id) { return vm_areas[app_id]; }

    void setPageTableType(String type) { page_table_type = type; }
    void setPageTableName(String name) { page_table_name = name; }
    String getPageTableType() { return page_table_type; }
    String getPageTableName() { return page_table_name; }


    int getNumberOfPageSizes() { return number_of_page_sizes; }
    int* getPageSizeList() { return page_size_list; }

    PageFaultHandlerBase *getPageFaultHandler() { return page_fault_handler; }
    SubsecondTime getPageFaultLatency() { return m_page_fault_latency.getLatency(); }

};