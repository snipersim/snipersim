#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// Agile TLB Prefetcher (ATP) + Sampling-Based Free TLB Prefetching (SBFP)
// ═══════════════════════════════════════════════════════════════════════════
//
// Implements the prefetcher described in:
//   "Exploiting Page Table Locality for Agile TLB Prefetching"
//
// ─── High-level overview ─────────────────────────────────────────────────
//
// ATP dynamically selects among three child TLB prefetchers:
//   P0 = H2P  (History-2-based Predictor: uses the last 3 miss VPNs)
//   P1 = MASP (Modified Arbitrary Stride Predictor: PC-indexed stride table)
//   P2 = STP  (Simple Target Predictor: predicts ±1 and ±2 neighbor pages)
//
// Selection uses three saturating counters (enable_pref, select_1, select_2)
// updated via a truth-table (Figure 7 in the paper) that compares which
// children *would have* predicted the current demand miss.  Each child
// maintains a Fake Prefetch Queue (FPQ) populated with its hypothetical
// predictions; a hit in child i's FPQ means "child i would have caught
// this miss".  Only the selected child's predictions trigger real page
// walks; the other two only update their FPQs.
//
// SBFP runs after every completed page walk (demand or prefetch).  A 64B
// cache line holds 8 leaf PTEs (at the 4KB page level), so fetching one
// PTE exposes up to 7 neighbors "for free".  A 14-entry Free Distance
// Table (FDT) with saturating counters tracks which neighbor distances
// tend to be useful.  Neighbors whose FDT counter exceeds a threshold
// are inserted into the real Prefetch Queue (PQ); the rest go into a
// 64-entry Sampler FIFO used for FDT training.
//
// ─── Key data structures ─────────────────────────────────────────────────
//
//   PQ       – 64-entry fully-associative FIFO of prefetched translations.
//   Sampler  – 64-entry FIFO of (VPN, free_distance) for FDT training.
//   FDT[14]  – one 10-bit saturating counter per free distance in
//              {-7..-1, +1..+7}. Decay: right-shift all on saturation.
//   FPQ×3    – 16-entry FIFO per child, holding VPNs the child *would*
//              have prefetched (including SBFP free neighbors).
//   MASP tbl – 64-entry 4-way set-assoc PC table for stride learning.
//
// ─── Per-miss flow (performPrefetch) ────────────────────────────────────
//
//   1. Probe PQ → hit: return translation, credit FDT if free prefetch.
//   2. PQ miss  → probe Sampler (off critical path), credit FDT on hit.
//   3. PQ miss  → issue demand page walk, run SBFP on completion.
//   4. Probe all 3 FPQs for current VPN.
//   5. Update ATP counters via truth table.
//   6. Select one child (or disable).
//   7. Issue real prefetch walks for selected child's predictions;
//      run SBFP after each successful prefetch walk.
//   8. Update all 3 FPQs with each child's fake predictions +
//      SBFP-admitted free neighbors from fake walks.
//   9. Update H2P history and MASP stride table.
//
// ═══════════════════════════════════════════════════════════════════════════

#include "tlb_prefetcher_base.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "shmem_perf_model.h"
#include "pagetable.h"
#include "cache_block_info.h"
#include "trans_defs.h"
#include "stats.h"
#include <vector>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace ParametricDramDirectoryMSI
{

class TLB;

class AgileTLBPrefetcher : public TLBPrefetcherBase
{
public:
	// ── ATP child selector ──────────────────────────────────────────
	enum ATPChild { DISABLE = 0, P0_H2P = 1, P1_MASP = 2, P2_STP = 3 };

	// ── Nested structures ───────────────────────────────────────────

	struct SamplerEntry {
		bool     valid;
		uint64_t vpn;
		int8_t   free_distance;
	};

	struct FPQEntry {
		bool     valid;
		uint64_t vpn;
	};

	struct MASPEntry {
		bool     valid;
		uint64_t pc_tag;
		uint64_t last_miss_vpn;
		int64_t  stride;
		bool     stride_valid;
	};

	// ── Statistics ──────────────────────────────────────────────────
	struct ATPStats {
		// ATP decision
		UInt64 queries;
		UInt64 pq_hits;
		UInt64 pq_misses;
		UInt64 sampler_hits;
		UInt64 sampler_misses;
		UInt64 enable_pref_inc;
		UInt64 enable_pref_dec;
		UInt64 select1_inc;
		UInt64 select1_dec;
		UInt64 select2_inc;
		UInt64 select2_dec;
		UInt64 atp_choice_disable;
		UInt64 atp_choice_h2p;
		UInt64 atp_choice_masp;
		UInt64 atp_choice_stp;

		// Real prefetch issue
		UInt64 real_prefetch_candidates_h2p;
		UInt64 real_prefetch_candidates_masp;
		UInt64 real_prefetch_candidates_stp;
		UInt64 real_prefetch_walks_issued;
		UInt64 real_prefetch_successful;
		UInt64 real_prefetch_failed;

		// SBFP
		UInt64 fdt_updates_from_pq;
		UInt64 fdt_updates_from_sampler;
		UInt64 fdt_decay_events;
		UInt64 free_candidates_seen;
		UInt64 free_inserted_pq;
		UInt64 free_inserted_sampler;

		// Per-distance stats: index 0..13 maps to distances -7..-1, +1..+7
		UInt64 dist_seen[14];
		UInt64 dist_pq[14];
		UInt64 dist_sampler[14];
		UInt64 dist_hits[14];

		// FPQ
		UInt64 fpq_p0_hits;
		UInt64 fpq_p1_hits;
		UInt64 fpq_p2_hits;
		UInt64 fpq_p0_inserts;
		UInt64 fpq_p1_inserts;
		UInt64 fpq_p2_inserts;
		UInt64 fpq_fake_base_preds_p0;
		UInt64 fpq_fake_base_preds_p1;
		UInt64 fpq_fake_base_preds_p2;
		UInt64 fpq_fake_free_preds_p0;
		UInt64 fpq_fake_free_preds_p1;
		UInt64 fpq_fake_free_preds_p2;

		// Demand walk
		UInt64 demand_walks;
		UInt64 demand_walks_successful;
		UInt64 demand_walks_failed;
	};

	// ── Constructor / destructor ────────────────────────────────────
	AgileTLBPrefetcher(Core *_core, MemoryManagerBase *_memory_manager,
					   ShmemPerfModel *_shmem_perf_model, String name,
					   uint32_t pq_size, uint32_t sampler_size,
					   uint32_t fpq_size, uint32_t fdt_counter_bits,
					   uint16_t fdt_threshold,
					   uint32_t enable_pref_bits, uint32_t select1_bits,
					   uint32_t select2_bits,
					   uint32_t masp_entries, uint32_t masp_assoc,
					   uint32_t page_shift,
					   uint32_t masp_lookahead = 1, uint32_t masp_degree = 1);
	~AgileTLBPrefetcher() override;

	// ── Main entry point ────────────────────────────────────────────
	std::vector<query_entry> performPrefetch(
		IntPtr address, IntPtr eip, Core::lock_signal_t lock,
		bool modeled, bool count, PageTable *pt,
		bool instruction = false, bool tlb_hit = false,
		bool pq_hit = false, int page_size = 12) override;

private:
	// ── Configuration ───────────────────────────────────────────────
	uint32_t m_sampler_size;
	uint32_t m_fpq_size;
	uint32_t m_fdt_counter_bits;
	uint16_t m_fdt_max;            // (1 << fdt_counter_bits) - 1
	uint16_t m_fdt_threshold;
	uint32_t m_enable_pref_bits;
	uint32_t m_select1_bits;
	uint32_t m_select2_bits;
	uint8_t  m_enable_pref_max;
	uint8_t  m_select1_max;
	uint8_t  m_select2_max;
	uint32_t m_page_shift;         // 12 for 4KB pages

	// ── Real structures ─────────────────────────────────────────────
	std::vector<SamplerEntry> m_sampler;
	uint16_t                  m_fdt[14]; // 14 saturating counters

	// SBFP metadata: maps VPN → free_distance for FDT credit on PQ hit.
	// Entries are inserted when a free neighbor is pushed to the TLB's PQ
	// and removed when consumed (pq_hit) or evicted by size cap.
	std::unordered_map<uint64_t, int8_t> m_free_meta;

	// Tracks VPNs that we've pushed to the TLB's PQ but haven't been
	// consumed yet.  Prevents duplicate prefetch walks for the same VPN.
	std::unordered_set<uint64_t> m_pending_vpns;

	// ── ATP meta-state ──────────────────────────────────────────────
	uint8_t  m_enable_pref;
	uint8_t  m_select1;
	uint8_t  m_select2;

	// ── Fake prefetch queues ────────────────────────────────────────
	std::vector<FPQEntry> m_fpq_p0; // H2P
	std::vector<FPQEntry> m_fpq_p1; // MASP
	std::vector<FPQEntry> m_fpq_p2; // STP

	// ── FIFO replacement pointers ───────────────────────────────────
	uint32_t m_sampler_fifo_ptr;
	uint32_t m_fpq_p0_fifo_ptr;
	uint32_t m_fpq_p1_fifo_ptr;
	uint32_t m_fpq_p2_fifo_ptr;

	// ── H2P state ───────────────────────────────────────────────────
	uint64_t m_h2p_last1_vpn;   // most recent miss
	uint64_t m_h2p_last2_vpn;   // second most recent miss
	bool     m_h2p_have1;
	bool     m_h2p_have2;

	// ── MASP state ──────────────────────────────────────────────────
	std::vector<MASPEntry> m_masp_table;
	uint32_t               m_masp_sets;
	uint32_t               m_masp_assoc;
	std::vector<uint32_t>  m_masp_rr_ptr;  // per-set round-robin pointer
	uint32_t               m_masp_lookahead;  // first prefetch at VPN + lookahead*stride
	uint32_t               m_masp_degree;     // number of stride steps prefetched

	// ── Bookkeeping ─────────────────────────────────────────────────
	bool m_current_instruction;

	// ── Stats ───────────────────────────────────────────────────────
	ATPStats m_stats;
	void registerAllStats(core_id_t core_id);

	// ── Saturating arithmetic helpers ───────────────────────────────
	template<typename T>
	static inline void satInc(T &v, T maxv) { if (v < maxv) ++v; }
	template<typename T>
	static inline void satDec(T &v)         { if (v > 0)    --v; }
	static inline bool msb(uint32_t v, uint32_t width)
	{ return (v >> (width - 1)) & 1; }

	// ── FDT helpers ─────────────────────────────────────────────────
	static inline uint32_t fdtIndexFromDistance(int8_t dist);
	void incrementFDTForDistance(int8_t dist);
	void decayFDTIfNeeded();

	// ── PQ operations ───────────────────────────────────────────────
	// (ATP uses the TLB subsystem's PQ; these helpers manage the
	//  free-meta and pending-vpn bookkeeping structures.)
	void recordFreeMeta(uint64_t vpn, int8_t free_distance);
	bool lookupAndConsumeFreeMeta(uint64_t vpn, int8_t &free_distance_out);
	bool isPending(uint64_t vpn) const;
	void markPending(uint64_t vpn);
	void consumePending(uint64_t vpn);

	// ── Sampler operations ──────────────────────────────────────────
	bool samplerLookup(uint64_t vpn, SamplerEntry &out, uint32_t &idx);
	void samplerInstall(uint64_t vpn, int8_t free_distance);

	// ── FPQ operations ──────────────────────────────────────────────
	bool fpqLookup(const std::vector<FPQEntry> &fpq, uint64_t vpn) const;
	void fpqInstall(std::vector<FPQEntry> &fpq, uint32_t &fifo_ptr, uint64_t vpn);

	// ── TLB containment ─────────────────────────────────────────────
	bool inAnyTLB(uint64_t vpn) const;

	// ── Direct page table lookup ────────────────────────────────────
	bool directPageTableLookupVPN(PageTable *pt, uint64_t vpn,
								  uint64_t &ppn, uint32_t &page_size) const;

	// ── Predictors ──────────────────────────────────────────────────
	std::vector<uint64_t> predictH2P(uint64_t vpn) const;
	std::vector<uint64_t> predictMASP(uint64_t vpn, uint64_t eip) const;
	std::vector<uint64_t> predictSTP(uint64_t vpn) const;

	// ── History / table updates ─────────────────────────────────────
	void updateH2PHistory(uint64_t vpn);
	void updateMASP(uint64_t vpn, uint64_t eip);
	uint32_t maspSetIndex(uint64_t eip) const;

	// ── SBFP free-neighbor processing ───────────────────────────────
	void processFreePTEsFromCompletedWalk(
		uint64_t demand_vpn, uint64_t demand_ppn, uint32_t demand_page_size,
		PageTable *pt, bool insert_into_real_pq,
		std::vector<uint64_t> *fake_targets_out,
		SubsecondTime walk_completion_time = SubsecondTime::Zero(),
		std::vector<query_entry> *result_out = nullptr);

	std::vector<uint64_t> getSBFPAdmittedFreeVPNs(uint64_t vpn, PageTable *pt);

	// ── ATP counter truth table update ──────────────────────────────
	void updateATPCounters(bool h0, bool h1, bool h2);

	// ── ATP child selection ─────────────────────────────────────────
	ATPChild chooseATPChild() const;
};

} // namespace ParametricDramDirectoryMSI
