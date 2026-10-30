#pragma once // Ensure this header file is included only once in compilation

#include "mmu.h"
#include "mmu_base.h"
#include "config.hpp"
#include "mmu_spec.h"
#include "spec_engine_base.h"
#include "tlb.h"
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "shmem_perf_model.h"
#include "pagetable.h"
#include "cache_block_info.h"
#include "stats.h"
#include "bloom_filter.h"
#include "city.h"

namespace ParametricDramDirectoryMSI
{
    /**
     * @class RevelatorOpenAddressingEngine
     * @brief A speculative prefetching engine that uses a single hash and open addressing (linear probing) to generate prefetch candidates.
     */
    class RevelatorOpenAddressingEngine : public SpecEngineBase
    {
    protected:
        String name; ///< Name of the speculative engine instance.
        bool oracle_revelator;
        bool oracle_revelator_filter;
        int m_probing_depth; // Replaces number_of_hashes and number_of_predictions
        int rev_type;
        UInt64 m_memory_size;
        UInt64 kernel_size;
        UInt64 m_total_pages;
        UInt64 m_kernel_pages;
        std::vector<bloom_filter *> m_bloom_filters;
        // Revelator-specific stats per probe attempt
        UInt64 *hits_per_probe;
        UInt64 *prefetches_per_probe;
        UInt64 *hits_per_probe_pt;
        UInt64 *prefetches_per_probe_pt;
        UInt64 *filtered_predictions_per_probe;
        bool has_filter;

        bool perfect_filtering;

        UInt64 relevator_prefetches;
        MemoryManagerBase *memory_manager; ///< Pointer to the memory manager instance.

        std::string log_file_name; ///< Name of the log file for output.
        std::ofstream log_file;    ///< Output stream for the log file.

    public:
        /**
         * @brief Constructor for RevelatorOpenAddressingEngine.
         *
         * @param core Pointer to the core instance associated with this engine.
         * @param _memory_manager Pointer to the memory manager handling memory operations.
         * @param shmem_perf_model Pointer to the shared memory performance model.
         * @param _name A string representing the name of this engine instance.
         */
        RevelatorOpenAddressingEngine(Core *core, MemoryManagerBase *_memory_manager, ShmemPerfModel *shmem_perf_model, String _name);

        /**
         * @brief Invokes the speculative engine for handling memory access.
         *
         * @param address The memory address to be accessed.
         * @param count Number of accesses to be performed.
         * @param lock Lock signal type associated with the access.
         * @param eip Instruction pointer address.
         * @param modeled Boolean flag indicating whether the access is modeled.
         */
        using SpecEngineBase::invokeSpecEngine;   // the visited-page-table overload stays visible
        void invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled, SubsecondTime invoke_start_time, IntPtr physical_address, bool page_table_speculation = false);

        /**
         * @brief Allocates an entry in the speculative engine. (Not used in this model)
         */
        void allocateInSpecEngine(IntPtr address, IntPtr ppn, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled) {
            // empty - no functionality needed
        };

        /**
         * @brief A single hash function to calculate an initial page index.
         * @param address The value to hash.
         * @param table_size The size of the hash table (total pages).
         * @return A hash value within the table size.
         */
        UInt64 hashFunction(IntPtr address, int table_size);
        
        /**
         * @brief Predicts potential physical addresses using open addressing.
         * @param address The virtual address that triggered the prediction.
         * @param is_page_table Flag indicating if the address is for a page table.
         * @return A vector of predicted physical addresses.
         */
        std::vector<IntPtr> predict(IntPtr address, bool is_page_table = false);
    };
}