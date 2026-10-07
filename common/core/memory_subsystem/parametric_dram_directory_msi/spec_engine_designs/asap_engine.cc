#include "asap_engine.h"
#include "simulator.h"
#include "cache_cntlr.h"
#include "memory_manager.h"
#include "stats.h"
#include "debug_config.h"

namespace ParametricDramDirectoryMSI
{
    ASAP::ASAP(Core *core, MemoryManagerBase *_memory_manager, ShmemPerfModel *shmem_perf_model, String _name)
        : SpecEngineBase(core, _memory_manager, shmem_perf_model, _name),
          m_last_spec_completion(SubsecondTime::Zero()),
          m_last_prediction_correct(false)
    {
        // The engines of the speculative MMU log under its gate.
        m_log = new SimLog("ASAP", core->getId(), DEBUG_MMU_SPEC);
        m_log->log("Initializing ASAP");

        stats.hits = 0;
        stats.invocations = 0;
        registerStatsMetric("asap", core->getId(), "asap_hits", &stats.hits);
        registerStatsMetric("asap", core->getId(), "asap_invocations", &stats.invocations);
    }

    ASAP::~ASAP()
    {
        delete m_log;
    }

    void ASAP::invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled, SubsecondTime invoke_start_time, IntPtr physical_address, bool page_table_speculation)
    {
        // The data-side invocation carries no visited entries: nothing to prefetch.
        m_last_spec_completion = SubsecondTime::Zero();
        m_last_prediction_correct = false;
    }

    void ASAP::invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled, SubsecondTime invoke_start_time, IntPtr physical_address, const accessedAddresses& visited_pts, bool page_table_speculation)
    {
        m_last_spec_completion = SubsecondTime::Zero();
        m_last_prediction_correct = false;

        if (!page_table_speculation)
            return;

        stats.invocations++;
        m_log->debug("Invoking ASAP for address ", SimLog::hex(address), " with ", visited_pts.size(), " visited entries");

        auto *l2_cntlr = memory_manager->getCacheCntlrAt(core->getId(), MemComponent::component_t::L2_CACHE);

        // The last two levels of the walk (PD at depth 2, PT at depth 3): their
        // lines are fetched now, in parallel with the first two levels.
        for (const auto &pt_access : visited_pts)
        {
            if (pt_access.depth < 2)
                continue;

            IntPtr cache_address = pt_access.physical_addr & ~((IntPtr)(64 - 1));
            m_log->trace("Prefetching depth ", pt_access.depth, " entry at ", SimLog::hex(pt_access.physical_addr), " line ", SimLog::hex(cache_address));

            l2_cntlr->handleMMUPrefetch(eip, cache_address, invoke_start_time, CacheBlockInfo::block_type_t::PAGE_TABLE_DATA);
            stats.hits++;

            // The lines are the walk's own, so the prediction is right by
            // construction; what the MMU scores is when the last one arrived.
            SubsecondTime completion = l2_cntlr->getLastPrefetchCompletion();
            if (completion > m_last_spec_completion)
                m_last_spec_completion = completion;
            m_last_prediction_correct = true;
        }
    }
}
