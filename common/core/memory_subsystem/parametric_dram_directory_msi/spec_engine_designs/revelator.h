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
     * @class SpecTLB
     * @brief A specialized speculative execution engine for handling speculative TLB lookups.
     */
    class Revelator : public SpecEngineBase
    {
    protected:
        String name; ///< Name of the speculative engine instance.
        bool m_disabled; ///< True when allocator is incompatible (no-op mode).
        bool oracle_revelator;
        bool oracle_revelator_filter;
        int number_of_hashes;
        int number_of_predictions;
        int rev_type ;
        UInt64 m_memory_size;
        UInt64 kernel_size;
        UInt64 m_total_pages;
        UInt64 m_kernel_pages;
        
        // Revelator-specific stats
        std::vector<bloom_filter *> m_bloom_filters;
        UInt64 *hits_per_hash;
        UInt64 *prefetches_per_hash;
        UInt64 *hits_per_hash_pt;
        UInt64 *prefetches_per_hash_pt;
        UInt64 *filtered_predictions_per_hash;
        bool has_filter;

        bool perfect_filtering;

        // Bloom filter stats - not used in Revelator but we did heavy testing with it - nothing worked well
        UInt64 *true_positive_per_bloom;
        UInt64 *false_positive_per_bloom;
        UInt64 *true_negative_per_bloom;
        UInt64 *false_negative_per_bloom;
        UInt64 *insertions_per_bloom;

        UInt64 relevator_prefetches;
        MemoryManagerBase *memory_manager; ///< Pointer to the memory manager instance.

        std::string log_file_name; ///< Name of the log file for output.
        std::ofstream log_file;    ///< Output stream for the log file.

        // Timing experiment: track spec completion for PTW-vs-spec delta analysis
        SubsecondTime m_last_spec_completion;   ///< When the last correct (or earliest) prefetch completed
        bool m_last_prediction_correct;          ///< Whether the last invocation had a correct prediction

        // Late mispredict tracking: wrong predictions whose prefetch arrived after PTW resolved
        UInt64 m_late_mispredict_data;            ///< Late wrong prefetches (data speculation)
        UInt64 m_late_mispredict_pt;              ///< Late wrong prefetches (page-table speculation)

    public:
        /**
         * @brief Constructor for SpecTLB.
         *
         * Initializes a SpecTLB instance for speculative TLB lookups.
         *
         * @param core Pointer to the core instance associated with this speculative TLB.
         * @param _memory_manager Pointer to the memory manager handling memory operations.
         * @param shmem_perf_model Pointer to the shared memory performance model.
         * @param _name A string representing the name of this SpecTLB instance.
         */
        Revelator(Core *core, MemoryManagerBase *_memory_manager, ShmemPerfModel *shmem_perf_model, String _name);

        /**
         * @brief Invokes the speculative engine for handling memory access.
         *
         * @param address The memory address to be accessed.
         * @param count Number of accesses to be performed.
         * @param lock Lock signal type associated with the access.
         * @param eip Instruction pointer address.
         * @param modeled Boolean flag indicating whether the access is modeled.
         */
        void invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled, SubsecondTime invoke_start_time, IntPtr physical_address, bool page_table_speculation = false);

        /**
         * @brief Allocates an entry in the speculative TLB.
         *
         * @param address The virtual memory address.
         * @param ppn The physical page number associated with the virtual address.
         * @param count Number of accesses to be performed.
         * @param lock Lock signal type associated with the allocation.
         * @param eip Instruction pointer address.
         * @param modeled Boolean flag indicating whether the allocation is modeled.
         */
        void allocateInSpecEngine(IntPtr address, IntPtr ppn, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled) {
            // empty - no functionality needed
        };
        UInt64 hashFunction(IntPtr address, int table_size);
        std::vector<IntPtr> predict(IntPtr address, int number_of_predictions, bool is_page_table = false);

        // Override timing accessors from SpecEngineBase
        SubsecondTime getLastSpecCompletionTime() const override { return m_last_spec_completion; }
        bool wasLastPredictionCorrect() const override { return m_last_prediction_correct; }
    };
}
