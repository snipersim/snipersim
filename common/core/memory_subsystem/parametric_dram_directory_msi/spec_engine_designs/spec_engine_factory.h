#pragma once // Ensure this header file is included only once in compilation

#include "mmu.h"
#include "mmu_base.h"
#include "config.hpp"
#include "mmu_spec.h"
#include "spec_engine_base.h"
#include "revelator.h"
#include "revelator_open_addressing_engine.h"
#include "revelator_thp_engine.h"
#include "numa_revelator.h"
// Step 8: ASAP
#include "asap_engine.h"
// Step 9: oracle
#include "oracle_spec.h"

namespace ParametricDramDirectoryMSI
{
    /**
     * @class SpecEngineFactory
     * @brief Factory class for creating different types of Speculative Execution Engines.
     */
    class SpecEngineFactory
    {
    public:
        /**
         * @brief Creates a Speculative Execution Engine (SpecEngineBase) based on the specified type.
         *
         * This function dynamically allocates and returns an instance of the appropriate
         * SpecEngineBase subclass based on the given type. If the type is invalid, the program aborts.
         *
         * @param type The type of Speculative Engine to create (e.g., "specTLB").
         * @param core Pointer to the Core instance that this speculative engine will be associated with.
         * @param memory_manager Pointer to the MemoryManager instance handling memory operations.
         * @param shmem_perf_model Pointer to the shared memory performance model.
         * @param name A string representing the name of the SpecEngine instance.
         *
         * @return Pointer to a dynamically allocated SpecEngineBase instance.
         */
        static SpecEngineBase *createSpecEngineBase(
            String type,
            Core *core,
            MemoryManagerBase *memory_manager,
            ShmemPerfModel *shmem_perf_model,
            String name)
        {
            if (type == "revelator")
            {
                return new Revelator(core, memory_manager, shmem_perf_model, name);
            }
            else if (type == "revelator_open_addressing")
            {
                return new RevelatorOpenAddressingEngine(core, memory_manager, shmem_perf_model, name);
            }
            else if (type == "revelator_thp")
            {
                return new RevelatorTHP(core, memory_manager, shmem_perf_model, name);
            }
            else if (type == "numa_revelator") {
                return new NumaRevelator(core, memory_manager, shmem_perf_model, name);
            }
            // Step 8: ASAP, the page-table-side prefetching engine.
            else if (type == "asap")
            {
                return new ASAP(core, memory_manager, shmem_perf_model, name);
            }
            // Step 9: the oracle data-side engine.
            else if (type == "oracle")
            {
                return new OracleSpec(core, memory_manager, shmem_perf_model, name);
            }
            // SpOT only if the deferred comparison engines are ever pulled in.
            else
            {
                std::cerr << "Invalid SpecEngine type: " << type << std::endl;
                abort();
            }
        }
    };
}
