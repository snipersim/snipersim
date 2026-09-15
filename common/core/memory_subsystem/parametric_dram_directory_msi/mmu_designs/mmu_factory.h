#pragma once

#include "mmu.h"
#include "mmu_base.h"
#include "mmu_spec.h"
#include "memory_manager_base.h"
#include "config.hpp"


namespace ParametricDramDirectoryMSI
{
    class MMUFactory
    {
    public:
        static MemoryManagementUnitBase *createMemoryManagementUnit(String type, Core *core, MemoryManagerBase *memory_manager, ShmemPerfModel *shmem_perf_model, String name, MemoryManagementUnitBase *nested_mmu = nullptr)
        {

			if (type == "default") // Baseline MMU design
			{
				return new MemoryManagementUnit(core, memory_manager, shmem_perf_model, name, nested_mmu);
			}
			else if (type == "spec") // MMU with a speculative-translation engine
			{
				return new MemoryManagementUnitSpec(core, memory_manager, shmem_perf_model, name, nested_mmu);
			}
			else
			{
				std::cerr << "Invalid MMU type: " << type << std::endl;
				abort();
			}
		}
	};
}
