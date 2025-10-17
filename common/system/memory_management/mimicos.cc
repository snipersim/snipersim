#include "mimicos.h"
#include "config.hpp"
#include "page_fault_handler_base.h"
#include "allocator_factory.h"
#include "pagetable_factory.h"
#include "handler_factory.h"
#include "dvfs_manager.h"
#include <string>

using namespace std;

MimicOS::MimicOS() : m_page_fault_latency(NULL, 0)
{


    std::cout << "[MimicOS]  OS is enabled" << std::endl;

    mimicos_name = "mimicos_host";
    page_table_type = Sim()->getCfg()->getString("perf_model/" + mimicos_name + "/page_table_type");
    page_table_name = Sim()->getCfg()->getString("perf_model/" + mimicos_name + "/page_table_name");

    m_memory_allocator = AllocatorFactory::createAllocator(mimicos_name);
    m_memory_allocator->fragment_memory();

    page_fault_handler = HandlerFactory::createHandler(Sim()->getCfg()->getString("perf_model/"+mimicos_name+"/page_fault_handler"), m_memory_allocator, mimicos_name);
    m_page_fault_latency = ComponentLatency(Sim()->getDvfsManager()->getGlobalDomain(), Sim()->getCfg()->getInt("perf_model/"+mimicos_name+"/page_fault_latency"));

    number_of_page_sizes = Sim()->getCfg()->getInt("perf_model/" + mimicos_name + "/number_of_page_sizes");
    page_size_list = new int[number_of_page_sizes];

    for (int i = 0; i < number_of_page_sizes; i++)
    {
        page_size_list[i] = Sim()->getCfg()->getIntArray("perf_model/" + mimicos_name + "/page_size_list", i);
    }

    std::cout << "[MimicOS] Page fault latency is " << m_page_fault_latency.getLatency().getNS() << " ns" << std::endl;
}

MimicOS::~MimicOS()
{
    delete m_memory_allocator;
}

void MimicOS::handle_page_fault(IntPtr address, IntPtr core_id, int frames)
{
    page_fault_handler->handlePageFault(address, core_id, frames);
}

void MimicOS::createApplication(int app_id)
{
    if (page_tables.find(app_id) != page_tables.end())
    {
        std::cout << "[MimicOS] Application " << app_id << " already exists" << std::endl;
        return;
    }

    std::cout << "[MimicOS] Creating application " << app_id << " with page table type " << page_table_type << " and name " << page_table_name << std::endl;

    // Create a new page table for the application
    ParametricDramDirectoryMSI::PageTable *page_table = ParametricDramDirectoryMSI::PageTableFactory::createPageTable(page_table_type, page_table_name, app_id);
    page_tables[app_id] = page_table;

    std::cout << "[MimicOS] Application " << app_id << " created successfully" << std::endl;

    return;
}
