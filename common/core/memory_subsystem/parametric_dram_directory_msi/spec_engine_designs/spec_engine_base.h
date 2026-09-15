#pragma once

// Include necessary header files
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "shmem_perf_model.h"
#include "pagetable.h"
#include "cache_block_info.h"
#include "stats.h"

namespace ParametricDramDirectoryMSI
{

    // Forward declaration of the TLBHierarchy class
    class TLBHierarchy;

    /**
     * @brief Base class for speculative engines, which manage speculative operations
     *        such as speculative TLB lookups and allocations.
     */
    class SpecEngineBase
    {

    protected:
        // Core instance associated with the speculative engine
        Core *core;

        // The memory manager to handle memory-related operations
        MemoryManagerBase *memory_manager;

        // The name of the speculative engine
        String name;

        // Shared memory performance model instance used for time tracking
        ShmemPerfModel *shmem_perf_model;

    public:
        /**
         * @brief Constructor for SpecEngineBase class.
         *
         * Initializes the SpecEngineBase with the core, memory manager, shared memory performance model, and name.
         *
         * @param core A pointer to the Core instance this speculative engine is associated with.
         * @param _memory_manager A pointer to the MemoryManager instance to handle memory operations.
         * @param shmem_perf_model A pointer to the ShmemPerfModel instance for tracking memory access time.
         * @param _name The name of the speculative engine.
         */
        SpecEngineBase(Core *core, MemoryManagerBase *_memory_manager, ShmemPerfModel *shmem_perf_model, String _name)
            : core(core), memory_manager(_memory_manager), name(_name), shmem_perf_model(shmem_perf_model)
        {
        }

        /**
         * @brief Abstract method to invoke speculative operations on a given address.
         *
         * This method should be implemented by subclasses to define the behavior when a speculative operation
         * is invoked on a given address. This typically includes actions like speculative TLB lookup.
         *
         * @param address The memory address being accessed speculatively.
         * @param count The number of times the memory access is being counted or performed.
         * @param lock The lock signal that controls if the speculative access is locked or unlocked.
         * @param eip The instruction pointer at the time of access (useful for certain types of speculation).
         * @param modeled A boolean flag that indicates whether the access is part of a modeled simulation.
         */
        virtual void invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled, SubsecondTime invoke_start_time, IntPtr physical_address, bool page_table_speculation = false) = 0;

        /**
         * @brief Overload that additionally receives the visited page-table entries.
         *
         * Default implementation ignores visited_pts and forwards to the original.
         * Subclasses (e.g. ASAP) may override to use real PT addresses.
         */
        virtual void invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled, SubsecondTime invoke_start_time, IntPtr physical_address, const accessedAddresses& visited_pts, bool page_table_speculation = false)
        {
            invokeSpecEngine(address, count, lock, eip, modeled, invoke_start_time, physical_address, page_table_speculation);
        }

        /**
         * @brief Abstract method to allocate entries in the speculative engine, typically in speculative TLB.
         *
         * This method should be implemented by subclasses to allocate an entry in the speculative structure
         * (e.g., speculative TLB) when there is a miss or the structure is not fully populated.
         *
         * @param address The memory address for which the allocation is being performed.
         * @param ppn The physical page number (PPN) associated with the address.
         * @param count The number of accesses or references to this address.
         * @param lock The lock signal controlling the allocation (whether it's locked or unlocked).
         * @param eip The instruction pointer at the time of access.
         * @param modeled A boolean flag indicating if this allocation is part of the modeled simulation.
         */
        virtual void allocateInSpecEngine(IntPtr address, IntPtr ppn, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled) = 0;

        /**
         * @brief Returns the completion time of the last speculative prefetch.
         *
         * After invokeSpecEngine is called, this returns the time at which the
         * (correct or earliest) speculative data became available in the cache.
         * Returns SubsecondTime::Zero() if no prefetch was issued.
         */
        virtual SubsecondTime getLastSpecCompletionTime() const { return SubsecondTime::Zero(); }

        /**
         * @brief Returns whether the last speculative prediction was correct.
         */
        virtual bool wasLastPredictionCorrect() const { return false; }
    };
}
