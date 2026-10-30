#pragma once

#include "spec_engine_base.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "sim_log.h"

namespace ParametricDramDirectoryMSI
{
    /**
     * @class OracleSpec
     * @brief The oracle data-side engine of the speculative MMU: an upper bound
     *        on what address speculation can gain.
     *
     * On every data-side invocation it prefetches the line the translation
     * actually resolves to -- the MMU hands it the walk's physical address --
     * at the time the walk started, so the data arrives as if the translation
     * had been known at the TLB miss.  It predicts nothing on the page-table
     * side and trains nothing.
     */
    class OracleSpec : public SpecEngineBase
    {
    protected:
        SimLog *m_log;

        SubsecondTime m_last_spec_completion;   ///< When the prefetched data line arrived
        bool m_last_prediction_correct;         ///< Always right once it has prefetched

        struct
        {
            UInt64 invocations;   ///< Data-side invocations
            UInt64 prefetches;    ///< Data lines prefetched (one per invocation)
        } stats;

    public:
        OracleSpec(Core *core, MemoryManagerBase *_memory_manager, ShmemPerfModel *shmem_perf_model, String _name);
        ~OracleSpec();

        using SpecEngineBase::invokeSpecEngine;   // the visited-page-table overload stays visible
        void invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled, SubsecondTime invoke_start_time, IntPtr physical_address, bool page_table_speculation = false) override;

        void allocateInSpecEngine(IntPtr address, IntPtr ppn, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled) override
        {
            // An oracle has nothing to learn.
        }

        SubsecondTime getLastSpecCompletionTime() const override { return m_last_spec_completion; }
        bool wasLastPredictionCorrect() const override { return m_last_prediction_correct; }
    };
}
