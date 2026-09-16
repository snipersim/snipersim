#pragma once

#include "tlb_prefetcher_base.h"
#include "RecencyPointerTable.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>

namespace ParametricDramDirectoryMSI
{

static constexpr uint64_t RECENCY_INVALID_VPN = ~uint64_t(0);

enum PredictionKind
{
	PRED_SAME,
	PRED_MINUS1,
	PRED_PLUS1
};

struct RecencyNode
{
	bool valid;
	uint64_t vpn;
	uint64_t ppn;
	uint64_t prev_vpn;
	uint64_t next_vpn;
	bool in_tlb;

	RecencyNode()
		: valid(false), vpn(0), ppn(0),
		  prev_vpn(RECENCY_INVALID_VPN), next_vpn(RECENCY_INVALID_VPN),
		  in_tlb(false) {}
};

class RecencyTLBPrefetcher : public TLBPrefetcherBase
{
public:
	RecencyTLBPrefetcher(Core *_core, MemoryManagerBase *_memory_manager,
						 ShmemPerfModel *_shmem_perf_model, String name,
						 uint32_t page_shift,
						 bool prefetch_same_recency,
						 bool prefetch_recency_minus_1,
						 bool prefetch_recency_plus_1,
						 bool prefetch_on_tlb_hit,
						 bool model_prefetch_walks,
					 bool consume_pq_on_hit,
					 bool model_pointer_chase);
	~RecencyTLBPrefetcher() override;

	std::vector<query_entry> performPrefetch(IntPtr address, IntPtr eip,
		Core::lock_signal_t lock, bool modeled, bool count,
		PageTable *pt, bool instruction = false,
		bool tlb_hit = false, bool pq_hit = false, int page_size = 12) override;

	void notifyVictim(IntPtr victim_address, int page_size, IntPtr ppn) override;

	void listRemove(uint64_t vpn);
	void listPushFront(uint64_t vpn);
	bool listContains(uint64_t vpn) const;
	// ── Stats registration ───────────────────────────────────────
	void registerAllStats(core_id_t core_id);
	// ── Node lookup / lazy creation ─────────────────────────────
	RecencyNode *getNode(uint64_t vpn);
	RecencyNode *getOrCreateNode(PageTable *pt, uint64_t vpn);

	// ── Direct PT lookup (no timing) ─────────────────────────────
	bool directPageTableLookupVPN(PageTable *pt, uint64_t vpn,
								  uint64_t &ppn, uint32_t &page_size) const;

	// ── Prediction helpers ───────────────────────────────────────
	void capturePredictionNeighbors(uint64_t vpn,
									uint64_t &same_vpn,
									uint64_t &minus1_vpn,
									uint64_t &plus1_vpn);

	void onTranslationInstalledIntoTLB(uint64_t demanded_vpn);
	void consumePendingVictim();

	bool inAnyTLB(uint64_t vpn) const;

	// ── Pointer-chase latency through cache hierarchy ────────────
	SubsecondTime modelPointerChase(uint64_t target_vpn, PageTable *pt,
									IntPtr eip, Core::lock_signal_t lock,
									bool modeled, bool count);

	query_entry makeQueryEntry(uint64_t vpn, uint64_t ppn,
							   uint32_t page_size,
							   SubsecondTime ts) const;

	void issuePredictionCandidate(uint64_t candidate_vpn,
								  IntPtr eip,
								  Core::lock_signal_t lock,
								  bool modeled, bool count,
								  PageTable *pt,
								  std::vector<query_entry> &result,
								 PredictionKind kind,
								 uint32_t extra_pointer_chases);

	uint64_t m_stack_head_vpn;   // recency list head (most-recent)
	uint64_t m_stack_tail_vpn;
	uint64_t m_pending_victim_vpn;  // buffered victim from notifyVictim()

	uint32_t m_page_shift;
	bool     m_prefetch_same_recency;
	bool     m_prefetch_recency_minus_1;
	bool     m_prefetch_recency_plus_1;
	bool     m_prefetch_on_tlb_hit;
	bool     m_model_prefetch_walks;
	bool     m_consume_pq_on_hit;
	bool     m_model_pointer_chase;  // if true, charge dynamic cache-modeled latency

	std::unordered_set<uint64_t> m_recently_predicted;
	static constexpr size_t MAX_RECENTLY_PREDICTED = 256;

	// ── Node store + radix pointer table ─────────────────────────
	std::unordered_map<uint64_t, RecencyNode> m_nodes;
	RecencyPointerTable m_pointer_table;  // separate radix table for prev/next pointers

	// ── Stats ────────────────────────────────────────────────────
	struct RecencyStats
	{
		// Core
		UInt64 queries;
		UInt64 queries_instruction;
		UInt64 queries_data;
		UInt64 tlb_hits;
		UInt64 tlb_misses;

		// PQ interaction
		UInt64 pq_hits_on_demand_miss;
		UInt64 pq_misses_on_demand_miss;
		UInt64 predictions_returned_to_pq;

		// Prediction stats
		UInt64 predictions_same_issued;
		UInt64 predictions_minus1_issued;
		UInt64 predictions_plus1_issued;
		UInt64 predictions_same_failed;
		UInt64 predictions_minus1_failed;
		UInt64 predictions_plus1_failed;
		UInt64 predictions_skipped_invalid;
		UInt64 predictions_skipped_tlb_resident;
		UInt64 predictions_skipped_pq_duplicate;

		// Recency structure
		UInt64 recency_unhooks;
		UInt64 recency_pushes;
		UInt64 recency_nodes_created;
		UInt64 recency_missing_node;
		UInt64 recency_victim_insertions;

		// Timing
		UInt64 prefetch_attempts;
		UInt64 prefetch_successful;
		UInt64 prefetch_failed;

		// Pointer-chase cache access stats
		UInt64 pointer_chase_accesses;
		UInt64 pointer_chase_pwc_hits;
		UInt64 pointer_chase_l1_hits;
		UInt64 pointer_chase_l2_hits;
		UInt64 pointer_chase_llc_hits;
		UInt64 pointer_chase_dram_hits;
	} m_stats;
};

} // namespace ParametricDramDirectoryMSI
