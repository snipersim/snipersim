#include "spot_engine.h"
#include "simulator.h"
#include "config.hpp"
#include "cache_cntlr.h"
#include "memory_manager.h"
#include "stats.h"
#include "mimicos.h"
#include "thread.h"
#include "debug_config.h"

#include <cmath>
#include <cstring>

namespace ParametricDramDirectoryMSI
{
    Spot::Spot(Core *core, MemoryManagerBase *_memory_manager, ShmemPerfModel *shmem_perf_model, String _name)
        : SpecEngineBase(core, _memory_manager, shmem_perf_model, _name),
          m_last_spec_completion(SubsecondTime::Zero()),
          m_last_prediction_correct(false)
    {
        m_log = new SimLog("SPOT", core->getId(), DEBUG_MMU_SPEC);

        // The table's geometry and the learning threshold sit with the
        // allocator's knobs, as upstream has them: the engine predicts the
        // placement that allocator makes.
        number_of_ways = Sim()->getCfg()->getInt("perf_model/spot_allocator/number_of_ways");
        number_of_sets = Sim()->getCfg()->getInt("perf_model/spot_allocator/number_of_sets");
        number_of_sets_bits = static_cast<int>(std::log2(number_of_sets));
        predefined_allocation_threshold = Sim()->getCfg()->getInt("perf_model/spot_allocator/predefined_allocation_threshold");
        cache.assign(number_of_sets, Ways(number_of_ways));

        m_log->log("SpOT engine: " + std::to_string(number_of_sets) + " sets x " + std::to_string(number_of_ways) +
                   " ways, learning after " + std::to_string(predefined_allocation_threshold) + " offset allocations");

        memset(&stats, 0, sizeof(stats));
        registerStatsMetric("spot", core->getId(), "cache_accesses", &stats.cache_accesses);
        registerStatsMetric("spot", core->getId(), "cache_hits", &stats.cache_hits);
        registerStatsMetric("spot", core->getId(), "cache_misses", &stats.cache_misses);
        registerStatsMetric("spot", core->getId(), "predictions_made", &stats.predictions_made);
        registerStatsMetric("spot", core->getId(), "prediction_not_made_saturation", &stats.prediction_not_made_saturation);
        registerStatsMetric("spot", core->getId(), "predictions_correct", &stats.predictions_correct);
        registerStatsMetric("spot", core->getId(), "prediction_accuracy", &stats.prediction_accuracy);
        registerStatsMetric("spot", core->getId(), "allocations", &stats.allocations);
        registerStatsMetric("spot", core->getId(), "no_allocation_confidence", &stats.no_allocation_confidence);
        registerStatsMetric("spot", core->getId(), "no_allocation_offset_mismatch", &stats.no_allocation_offset_mismatch);
        registerStatsMetric("spot", core->getId(), "evictions_zero_confidence", &stats.evictions_zero_confidence);
        registerStatsMetric("spot", core->getId(), "allocations_bypassed_confident_set", &stats.allocations_bypassed_confident_set);
        registerStatsMetric("spot", core->getId(), "allocation_in_invalid_way", &stats.allocation_in_invalid_way);
        registerStatsMetric("spot", core->getId(), "refreshed_hit_entry", &stats.refreshed_hit_entry);
    }

    Spot::~Spot()
    {
        delete m_log;
    }

    void Spot::invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled,
                                SubsecondTime invoke_start_time, IntPtr physical_address, bool page_table_speculation)
    {
        m_last_spec_completion = SubsecondTime::Zero();
        m_last_prediction_correct = false;

        // Data side only: the page-table-side invocation is a no-op.
        if (page_table_speculation)
            return;

        UInt64 index = eip % number_of_sets;
        UInt64 tag = eip >> number_of_sets_bits;
        int64_t vpn = static_cast<int64_t>(address >> 12);
        Ways &ways = cache[index];

        // One set lookup is one access, however many ways it has.
        stats.cache_accesses++;
        for (auto &entry : ways)
        {
            if (!entry.valid || entry.tag != tag)
                continue;
            entry.lru = ++global_lru;
            stats.cache_hits++;

            if (entry.saturation_counter <= 1)
            {
                stats.prediction_not_made_saturation++;
                m_log->debug("PC", SimLog::hex(eip), "hit with confidence", entry.saturation_counter, ": no prediction");
                return;
            }

            // Confident: prefetch the line at the predicted physical address.
            IntPtr predicted_pa = (static_cast<IntPtr>(vpn - entry.offset) << 12) | (address & 0xFFF);
            IntPtr cache_line = predicted_pa & ~((IntPtr)63);
            auto *l2_cntlr = memory_manager->getCacheCntlrAt(core->getId(), MemComponent::component_t::L2_CACHE);
            l2_cntlr->handleMMUPrefetch(eip, cache_line, invoke_start_time, CacheBlockInfo::block_type_t::DATA);
            m_last_spec_completion = l2_cntlr->getLastPrefetchCompletion();
            stats.predictions_made++;

            if (offsetMatches(address, physical_address, entry.offset))
            {
                stats.predictions_correct++;
                m_last_prediction_correct = true;
                if (entry.saturation_counter < 3)
                    entry.saturation_counter++;
            }
            else if (entry.saturation_counter > 0)
                entry.saturation_counter--;

            stats.prediction_accuracy = (UInt64)(100.0 * (double)stats.predictions_correct / (double)stats.predictions_made);
            m_log->debug("PC", SimLog::hex(eip), "predicted", SimLog::hex(predicted_pa), "for", SimLog::hex(address),
                         "real", SimLog::hex(physical_address), m_last_prediction_correct ? "correct" : "wrong");
            return;
        }
        stats.cache_misses++;
    }

    void Spot::allocateInSpecEngine(IntPtr virt_addr, IntPtr ppn, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled)
    {
        UInt64 index = eip % number_of_sets;
        UInt64 tag = eip >> number_of_sets_bits;
        int app_id = core->getThread()->getAppId();

        // Learn only an offset the allocator really placed this page at,
        // and only once the VMA has enough pages at it to trust.
        IntPtr offset_raw = Sim()->getMimicOS()->getPhysicalOffsetSpot(app_id, virt_addr);
        if (offset_raw == static_cast<IntPtr>(-1))
        {
            stats.no_allocation_offset_mismatch++;
            return;
        }
        int64_t offset = static_cast<int64_t>(offset_raw);
        if (!offsetMatches(virt_addr, ppn << 12, offset))
        {
            stats.no_allocation_offset_mismatch++;
            return;
        }
        int successful_allocs = Sim()->getMimicOS()->getSuccessfulOffsetBasedAllocations(app_id, virt_addr);
        if (successful_allocs <= predefined_allocation_threshold)
        {
            stats.no_allocation_confidence++;
            return;
        }

        Ways &ways = cache[index];

        // The PC's entry exists: refresh it.
        for (auto &entry : ways)
        {
            if (entry.valid && entry.tag == tag)
            {
                entry.offset = offset;
                entry.lru = ++global_lru;
                if (entry.saturation_counter < 3)
                    entry.saturation_counter++;
                stats.allocations++;
                stats.refreshed_hit_entry++;
                return;
            }
        }

        // An invalid way, else the least recently used way among those
        // with no confidence left; a set of confident entries is bypassed.
        int victim = -1;
        for (int i = 0; i < number_of_ways; i++)
            if (!ways[i].valid) { victim = i; break; }
        bool invalid_way = (victim != -1);
        if (!invalid_way)
        {
            UInt64 oldest = UINT64_MAX;
            for (int i = 0; i < number_of_ways; i++)
                if (ways[i].saturation_counter == 0 && ways[i].lru < oldest)
                {
                    oldest = ways[i].lru;
                    victim = i;
                }
        }
        if (victim == -1)
        {
            stats.allocations_bypassed_confident_set++;
            return;
        }

        SpotEntry fresh;
        fresh.valid = true;
        fresh.tag = tag;
        fresh.offset = offset;
        fresh.saturation_counter = 1;
        fresh.lru = ++global_lru;
        ways[victim] = fresh;
        stats.allocations++;
        if (invalid_way)
            stats.allocation_in_invalid_way++;
        else
            stats.evictions_zero_confidence++;
        m_log->debug("PC", SimLog::hex(eip), "learned offset", offset, "for", SimLog::hex(virt_addr));
    }
}
