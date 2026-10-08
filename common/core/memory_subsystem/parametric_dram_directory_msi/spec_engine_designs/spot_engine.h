#pragma once

#include "spec_engine_base.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "sim_log.h"
#include <vector>

namespace ParametricDramDirectoryMSI
{
    /**
     * @class Spot
     * @brief Speculative Offset-based Translation [Alverti et al., ISCA 2020]
     *        as a data-side engine of the speculative MMU.
     *
     * The SpOT allocator keeps VA - PA constant across a VMA.  The engine
     * caches that offset per load PC in a small set-associative table (the
     * PC's low bits index a set, the rest are the tag): on a TLB miss whose
     * PC hits a confident entry it prefetches the line at (VPN - offset)
     * into the L2 while the walk resolves the real translation, then scores
     * itself against the translation and moves the entry's 2-bit confidence.
     * After a walk the entry is learned, when the VMA's offset explains the
     * translation and the allocator has placed enough of that VMA's pages
     * at it (predefined_allocation_threshold); a set whose entries are all
     * confident is not evicted from (the paper's bypass).
     */
    class Spot : public SpecEngineBase
    {
    protected:
        struct SpotEntry
        {
            bool valid = false;
            UInt64 tag = 0;
            int64_t offset = 0;           ///< VPN - PPN of the VMA the PC touched
            int saturation_counter = 1;   ///< 2-bit confidence; predicts above 1
            UInt64 lru = 0;
        };
        using Ways = std::vector<SpotEntry>;

        SimLog *m_log;
        int number_of_sets;
        int number_of_sets_bits;
        int number_of_ways;
        int predefined_allocation_threshold;
        UInt64 global_lru = 0;
        std::vector<Ways> cache;

        SubsecondTime m_last_spec_completion;
        bool m_last_prediction_correct;

        struct
        {
            UInt64 cache_accesses;
            UInt64 cache_hits;
            UInt64 cache_misses;
            UInt64 predictions_made;
            UInt64 prediction_not_made_saturation;
            UInt64 predictions_correct;
            UInt64 prediction_accuracy;
            UInt64 allocations;
            UInt64 no_allocation_confidence;
            UInt64 no_allocation_offset_mismatch;
            UInt64 evictions_zero_confidence;
            UInt64 allocations_bypassed_confident_set;
            UInt64 allocation_in_invalid_way;
            UInt64 refreshed_hit_entry;
        } stats;

        static bool offsetMatches(IntPtr va, IntPtr pa, int64_t offset)
        {
            return static_cast<int64_t>(va >> 12) - static_cast<int64_t>(pa >> 12) == offset;
        }

    public:
        Spot(Core *core, MemoryManagerBase *_memory_manager, ShmemPerfModel *shmem_perf_model, String _name);
        ~Spot();

        void invokeSpecEngine(IntPtr address, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled, SubsecondTime invoke_start_time, IntPtr physical_address, bool page_table_speculation = false) override;
        void allocateInSpecEngine(IntPtr address, IntPtr ppn, int count, Core::lock_signal_t lock, IntPtr eip, bool modeled) override;

        SubsecondTime getLastSpecCompletionTime() const override { return m_last_spec_completion; }
        bool wasLastPredictionCorrect() const override { return m_last_prediction_correct; }
    };
}
