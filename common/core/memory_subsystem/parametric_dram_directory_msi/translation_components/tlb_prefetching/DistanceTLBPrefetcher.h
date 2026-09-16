#pragma once

#include "tlb_prefetcher_base.h"
#include <vector>
#include <unordered_set>
#include <cstdint>

namespace ParametricDramDirectoryMSI
{

struct DPPredSlot
{
	bool    valid;
	int64_t predicted_distance;
	uint8_t lru;              // lower = more recently used

	DPPredSlot() : valid(false), predicted_distance(0), lru(0) {}
};

struct DPRow
{
	bool    valid;
	int64_t distance_tag;     // key: the current distance that indexes this row
	uint8_t row_lru;          // lower = more recently accessed (for set-associative replacement)
	std::vector<DPPredSlot> slots;

	DPRow() : valid(false), distance_tag(0), row_lru(0) {}
};

class DistanceTLBPrefetcher : public TLBPrefetcherBase
{
public:
	DistanceTLBPrefetcher(Core *_core, MemoryManagerBase *_memory_manager,
						  ShmemPerfModel *_shmem_perf_model, String name,
						  uint32_t page_shift, uint32_t num_rows,
						  uint32_t num_slots, uint32_t assoc,
						  bool model_prefetch_walks,
						  uint32_t max_depth);

	~DistanceTLBPrefetcher() override;

	std::vector<query_entry> performPrefetch(
		IntPtr address, IntPtr eip, Core::lock_signal_t lock,
		bool modeled, bool count, PageTable *pt,
		bool instruction = false, bool tlb_hit = false,
		bool pq_hit = false, int page_size = 12) override;

private:
	// ── Table helpers ─────────────────────────────────────────────
	uint32_t hashDistance(int64_t d) const;
	bool     lookupRow(int64_t distance, DPRow *&row);
	DPRow   *allocateRow(int64_t distance);
	void     updateSuccessorDistance(int64_t previous_distance,
									int64_t current_distance);
	void     touchSlotLRU(DPRow &row, uint32_t slot_idx);

	// ── Prefetch issue ───────────────────────────────────────────
	void issuePredictionsFromRow(const DPRow &row, uint64_t current_vpn,
								IntPtr eip, Core::lock_signal_t lock,
								bool modeled, bool count, PageTable *pt,
								std::vector<query_entry> &result);

	// ── PT / TLB helpers ─────────────────────────────────────────
	bool directPageTableLookupVPN(PageTable *pt, uint64_t vpn,
								  uint64_t &ppn, uint32_t &page_size) const;
	bool inAnyTLB(uint64_t vpn) const;

	// ── Stats registration ───────────────────────────────────────
	void registerAllStats(core_id_t core_id);

	// ── Configuration ────────────────────────────────────────────
	uint32_t m_page_shift;
	uint32_t m_num_rows;      // r (total rows = num_sets * assoc)
	uint32_t m_num_slots;     // s  predicted distances per row
	uint32_t m_assoc;         // 1 = direct-mapped
	uint32_t m_num_sets;      // m_num_rows / m_assoc

	// ── Prediction table ─────────────────────────────────────────
	std::vector<DPRow> m_table;   // size = m_num_rows

	// ── PQ duplicate suppression ─────────────────────────────────
	std::unordered_set<uint64_t> m_recently_predicted;
	static constexpr size_t MAX_RECENTLY_PREDICTED = 64;

	// ── Miss history ─────────────────────────────────────────────
	bool     m_have_last_miss;
	uint64_t m_last_miss_vpn;

	bool     m_have_last_distance;
	int64_t  m_last_distance;

	bool     m_model_prefetch_walks;
	uint32_t m_max_depth;     // 1 = original DP (no chaining); >1 = chained
	                          // Markov walk following the MRU successor

	// ── Stats ────────────────────────────────────────────────────
	struct DPStats
	{
		// Core
		UInt64 queries;
		UInt64 queries_instruction;
		UInt64 queries_data;
		UInt64 tlb_misses;

		// Distance history
		UInt64 first_miss_no_distance;
		UInt64 distance_computed;
		UInt64 zero_distance_seen;
		UInt64 positive_distance_seen;
		UInt64 negative_distance_seen;

		// Table behavior
		UInt64 table_lookups;
		UInt64 table_hits;
		UInt64 table_misses;
		UInt64 row_allocations;
		UInt64 row_replacements;

		// Successor-slot behavior
		UInt64 successor_updates;
		UInt64 successor_hits_existing;
		UInt64 successor_inserts_free;
		UInt64 successor_replaces_lru;

		// Prefetch issue
		UInt64 predictions_issued;
		UInt64 predictions_skipped_invalid_vpn;
		UInt64 predictions_skipped_tlb_resident;
		UInt64 predictions_skipped_pq_duplicate;
		UInt64 prefetch_attempts;
		UInt64 prefetch_successful;
		UInt64 prefetch_failed;

		// Chained-depth walk (max_depth > 1)
		UInt64 chain_lookups;            // depth>=2 row lookups attempted
		UInt64 chain_row_hits;           // depth>=2 lookups that found a row
		UInt64 chain_breaks_no_row;      // chain ended: successor distance had no row
		UInt64 chain_breaks_no_slot;     // chain ended: row had no valid MRU slot
		UInt64 chain_breaks_invalid_vpn; // chain ended: advanced VPN went negative
		UInt64 chain_predictions_issued; // predictions issued at depth >= 2
	} m_stats;
};

} // namespace ParametricDramDirectoryMSI
