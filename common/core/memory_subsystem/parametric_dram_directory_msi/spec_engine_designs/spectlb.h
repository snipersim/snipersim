#pragma once

#include "spec_engine_base.h"
#include "tlb.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "cache_block_info.h"
#include "sim_log.h"

namespace ParametricDramDirectoryMSI
{
    /**
     * @class SpecTLB
     * @brief SpecTLB [Barr, Cox and Rixner, ISCA 2011] as a data-side engine of
     *        the speculative MMU.
     *
     * The reservation-based THP allocator hands out 4 KB pages from a reserved
     * 2 MB region, so every page of a reserved region sits at the same offset
     * in physical memory as in virtual memory.  SpecTLB keeps a small TLB of
     * those regions (one entry per reserved 2 MB block, tagged at 2 MB): on a
     * TLB miss it looks the address up there and, on a hit, prefetches the
     * data line at the speculative physical address -- region base plus the
     * offset within 2 MB -- while the walk resolves the real one.  A miss in
     * the speculative TLB allocates the region after the walk, if the
     * allocator reports it reserved.
     */
    class SpecTLB : public SpecEngineBase
    {
    protected:
        SimLog *m_log;
        TLB *spec_tlb;                          ///< The speculative TLB, tagged at 2 MB

        SubsecondTime spec_access_time;         ///< When the last lookup started
        bool spec_hit;                          ///< The last lookup hit
        CacheBlockInfo *spec_tlb_block_info;    ///< The entry the last lookup found

        SubsecondTime m_last_spec_completion;   ///< When the last prefetched line arrived
        bool m_last_prediction_correct;         ///< The speculative line was the real one

        struct
        {
            UInt64 invocations;         ///< Data-side invocations (speculative lookups)
            UInt64 hits;                ///< Lookups that hit and prefetched
            UInt64 correct_predictions; ///< Hits whose line was the translation's own
            UInt64 allocations;         ///< Reserved regions installed after a miss
        } stats;

    public:
        SpecTLB(Core *core, MemoryManagerBase *_memory_manager, ShmemPerfModel *shmem_perf_model, String _name);
        ~SpecTLB();

        using SpecEngineBase::invokeSpecEngine;   // the visited-page-table overload stays visible
        void invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled, SubsecondTime invoke_start_time, IntPtr physical_address, bool page_table_speculation = false) override;
        void allocateInSpecEngine(IntPtr address, IntPtr ppn, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled) override;

        SubsecondTime getLastSpecCompletionTime() const override { return m_last_spec_completion; }
        bool wasLastPredictionCorrect() const override { return m_last_prediction_correct; }
    };
}
