#include "oracle_spec.h"
#include "simulator.h"
#include "cache_cntlr.h"
#include "memory_manager.h"
#include "stats.h"
#include "debug_config.h"

namespace ParametricDramDirectoryMSI
{
    OracleSpec::OracleSpec(Core *core, MemoryManagerBase *_memory_manager, ShmemPerfModel *shmem_perf_model, String _name)
        : SpecEngineBase(core, _memory_manager, shmem_perf_model, _name),
          m_last_spec_completion(SubsecondTime::Zero()),
          m_last_prediction_correct(false)
    {
        m_log = new SimLog("ORACLE_SPEC", core->getId(), DEBUG_MMU_SPEC);
        m_log->log("Initializing the oracle spec engine");

        stats.invocations = 0;
        stats.prefetches = 0;
        registerStatsMetric("oracle", core->getId(), "invocations", &stats.invocations);
        registerStatsMetric("oracle", core->getId(), "prefetches", &stats.prefetches);
    }

    OracleSpec::~OracleSpec()
    {
        delete m_log;
    }

    void OracleSpec::invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled, SubsecondTime invoke_start_time, IntPtr physical_address, bool page_table_speculation)
    {
        m_last_spec_completion = SubsecondTime::Zero();
        m_last_prediction_correct = false;

        // Data only: the page-table-side invocation is left to the walk.
        if (page_table_speculation)
            return;

        stats.invocations++;
        m_log->debug("Oracle prefetch for address ", SimLog::hex(address), " -> ", SimLog::hex(physical_address));

        auto *l2_cntlr = memory_manager->getCacheCntlrAt(core->getId(), MemComponent::component_t::L2_CACHE);
        IntPtr cache_address = physical_address & ~((IntPtr)(64 - 1));
        l2_cntlr->handleMMUPrefetch(eip, cache_address, invoke_start_time, CacheBlockInfo::block_type_t::DATA);
        stats.prefetches++;

        m_last_spec_completion = l2_cntlr->getLastPrefetchCompletion();
        m_last_prediction_correct = true;
    }
}
