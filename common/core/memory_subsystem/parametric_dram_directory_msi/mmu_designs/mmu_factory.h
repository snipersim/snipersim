#pragma once

#include "mmu.h"
#include "mmu_base.h"
#include "mmu_spec.h"
#include "mmu_victima.h"
#include "mmu_hw_faults.h"
#include "mmu_virt.h"
#include "mmu_dmt.h"
#include "mmu_pomtlb.h"
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
			else if (type == "victima") // Victima: cache lines used as TLB blocks
			{
				return new MemoryManagementUnitVictima(core, memory_manager, shmem_perf_model, name, nested_mmu);
			}
			else if (type == "hw_fault") // Page faults handled in hardware
			{
				return new MemoryManagementUnitHWFault(core, memory_manager, shmem_perf_model, name, nested_mmu);
			}
			else if (type == "virt") // Baseline MMU design that supports virtualized execution (nested on a host MMU)
			{
				return new MemoryManagementUnitVirt(core, memory_manager, shmem_perf_model, name, nested_mmu);
			}
			else if (type == "dmt") // Direct Memory Translation: a walk is one access of the leaf entry
			{
				return new MemoryManagementUnitDMT(core, memory_manager, shmem_perf_model, name, nested_mmu);
			}
			else if (type == "pomtlb") // Part-of-Memory TLB: an in-memory TLB between the L2 TLB and the walker
			{
				return new MemoryManagementUnitPOMTLB(core, memory_manager, shmem_perf_model, name, nested_mmu);
			}
			else
			{
				std::cerr << "Invalid MMU type: " << type << std::endl;
				abort();
			}
		}
	};
}
