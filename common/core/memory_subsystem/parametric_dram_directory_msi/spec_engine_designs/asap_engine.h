#pragma once

#include "spec_engine_base.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "pagetable.h"
#include "sim_log.h"

namespace ParametricDramDirectoryMSI
{
    /**
     * @class ASAP
     * @brief The ASAP speculative page-table engine [Margaritov et al., Prefetched
     *        Address Translation, MICRO 2019], as a spec engine of the speculative MMU.
     *
     * ASAP overlaps the last two levels of a radix walk with the first two: on
     * every page-table-side invocation it prefetches, into the L2 cache, the
     * page-table lines of the entries the walk will reach at depth 2 (PD) and
     * depth 3 (PT).  The speculative MMU hands it the entries the functional
     * walk visited (initializeWalk's accesses, before PWC filtering), so the
     * addresses are the real ones; the timing effect is that those lines are
     * in flight before the walk's dependent accesses ask for them.
     *
     * The data-side invocation (no visited entries) is a no-op: ASAP predicts
     * page-table lines, not data.
     */
    class ASAP : public SpecEngineBase
    {
    protected:
        SimLog *m_log;

        SubsecondTime m_last_spec_completion;   ///< When the last prefetched line arrived
        bool m_last_prediction_correct;         ///< ASAP prefetches the walk's own lines

        struct
        {
            UInt64 hits;          ///< Page-table lines prefetched (one per depth >= 2 entry)
            UInt64 invocations;   ///< Page-table-side invocations
        } stats;

    public:
        ASAP(Core *core, MemoryManagerBase *_memory_manager, ShmemPerfModel *shmem_perf_model, String _name);
        ~ASAP();

        void invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled, SubsecondTime invoke_start_time, IntPtr physical_address, bool page_table_speculation = false) override;
        void invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled, SubsecondTime invoke_start_time, IntPtr physical_address, const accessedAddresses& visited_pts, bool page_table_speculation = false) override;

        void allocateInSpecEngine(IntPtr address, IntPtr ppn, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled) override
        {
            // Nothing to train: the prefetched lines come from the walk itself.
        }

        SubsecondTime getLastSpecCompletionTime() const override { return m_last_spec_completion; }
        bool wasLastPredictionCorrect() const override { return m_last_prediction_correct; }
    };
}
