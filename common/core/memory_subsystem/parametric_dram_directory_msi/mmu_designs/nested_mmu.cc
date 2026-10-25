

// #include "memory_manager.h"
// #include "cache_cntlr.h"
// #include "subsecond_time.h"
// #include "fixed_types.h"
// #include "core.h"
// #include "shmem_perf_model.h"
// #include "pagetable.h"
// #include "tlb_subsystem.h"
// #include "mmu_base.h"
// #include "metadata_table_base.h"
// #include "nested_mmu.h"
// #include "mmu_factory.h"
// //#define DEBUG_MMU

// namespace ParametricDramDirectoryMSI
// {



// 		MemoryManagementUnitNested::MemoryManagementUnitNested(Core *core, MemoryManager *memory_manager, ShmemPerfModel *shmem_perf_model, String _name, MemoryManagementUnitBase* _nested_mmu) :
//         MemoryManagementUnitBase(core, memory_manager, shmem_perf_model, _name, _nested_mmu)
//         {
            
//             name = _name;
//             String guest_mmu_name = "guest_mmu";
//             String host_mmu_name = "host_mmu";

//             host_mmu = MMUFactory::createMemoryManagementUnit("default", core, memory_manager, shmem_perf_model, host_mmu_name);
//             guest_mmu = MMUFactory::createMemoryManagementUnit("default", core, memory_manager, shmem_perf_model, guest_mmu_name, host_mmu);

//             registerMMUStats();

//         }

//         MemoryManagementUnitNested::~MemoryManagementUnitNested(){

//             delete guest_mmu;
//             delete host_mmu;

//         } 

//         void MemoryManagementUnitNested::registerMMUStats(){

//             bzero (&translation_stats, sizeof(translation_stats));

//             registerStatsMetric("nested_mmu", core->getId(), "total_translation_latency", &translation_stats.total_translation_latency);
//             registerStatsMetric("nested_mmu", core->getId(), "total_guest_translation_latency", &translation_stats.total_guest_translation_latency);
//             registerStatsMetric("nested_mmu", core->getId(), "total_host_translation_latency", &translation_stats.total_host_translation_latency);

//         }


// 		pair<SubsecondTime, IntPtr> MemoryManagementUnitNested::performAddressTranslation(IntPtr eip, IntPtr address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count)
//         {

// #ifdef DEBUG_MMU
//             std::cout << "Guest MMU needs to translate address: " << address << std::endl;
// #endif
//             SubsecondTime guest_latency = SubsecondTime::Zero();
    
//             auto guest_result = guest_mmu->performAddressTranslation(eip, address, instruction, lock, modeled, count);
            
//             guest_latency = guest_result.first;
//             IntPtr guest_physical_address = guest_result.second;

//             auto host_result = host_mmu->performAddressTranslation(eip, guest_physical_address, instruction, lock, modeled, count);
//             SubsecondTime host_latency = host_result.first;
//             IntPtr host_physical_address = host_result.second;


//             return make_pair(guest_latency+host_latency, host_physical_address);
//         }
	
//         void MemoryManagementUnitNested::discoverVMAs(){

//         }


// 	    pair<bool, pair<SubsecondTime, IntPtr>> MemoryManagementUnitNested::performAddressTranslationRevelator(IntPtr eip, IntPtr address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count){
// 		    std::cout << "Relevator not implemented for this MMU" << std::endl;
// 		    exit(1);
// 		    return std::make_pair(false, std::make_pair(SubsecondTime::Zero(), 0));

// 	    }
// }