// DistanceTLBPrefetcher.cc – Distance Prefetching (DP)
//
// Implements the distance-indexed predictor from "Going the Distance
// for TLB Prefetching."  The table is keyed by *distance* (delta between
// consecutive miss VPNs), not by PC or VPN.  Each row stores s predicted
// successor distances in LRU order.  Prefetch candidates are returned as
// query_entry objects so the simulator places them in the existing PQ;
// no private prefetch buffer is created.

#include "DistanceTLBPrefetcher.h"
#include "tlb.h"
#include "stats.h"
#include <algorithm>  // sort
#include <cassert>
#include <cstdlib>    // abs

namespace ParametricDramDirectoryMSI
{

// ═══════════════════════════════════════════════════════════════════
//  Stats registration
// ═══════════════════════════════════════════════════════════════════

void DistanceTLBPrefetcher::registerAllStats(core_id_t core_id)
{
	// Core
	registerStatsMetric("dp_tlb", core_id, "queries",                       &m_stats.queries);
	registerStatsMetric("dp_tlb", core_id, "queries_instruction",           &m_stats.queries_instruction);
	registerStatsMetric("dp_tlb", core_id, "queries_data",                 &m_stats.queries_data);
	registerStatsMetric("dp_tlb", core_id, "tlb_misses",                   &m_stats.tlb_misses);

	// Distance history
	registerStatsMetric("dp_tlb", core_id, "first_miss_no_distance",       &m_stats.first_miss_no_distance);
	registerStatsMetric("dp_tlb", core_id, "distance_computed",            &m_stats.distance_computed);
	registerStatsMetric("dp_tlb", core_id, "zero_distance_seen",           &m_stats.zero_distance_seen);
	registerStatsMetric("dp_tlb", core_id, "positive_distance_seen",       &m_stats.positive_distance_seen);
	registerStatsMetric("dp_tlb", core_id, "negative_distance_seen",       &m_stats.negative_distance_seen);

	// Table behavior
	registerStatsMetric("dp_tlb", core_id, "table_lookups",                &m_stats.table_lookups);
	registerStatsMetric("dp_tlb", core_id, "table_hits",                   &m_stats.table_hits);
	registerStatsMetric("dp_tlb", core_id, "table_misses",                 &m_stats.table_misses);
	registerStatsMetric("dp_tlb", core_id, "row_allocations",              &m_stats.row_allocations);
	registerStatsMetric("dp_tlb", core_id, "row_replacements",             &m_stats.row_replacements);

	// Successor-slot behavior
	registerStatsMetric("dp_tlb", core_id, "successor_updates",            &m_stats.successor_updates);
	registerStatsMetric("dp_tlb", core_id, "successor_hits_existing",      &m_stats.successor_hits_existing);
	registerStatsMetric("dp_tlb", core_id, "successor_inserts_free",       &m_stats.successor_inserts_free);
	registerStatsMetric("dp_tlb", core_id, "successor_replaces_lru",       &m_stats.successor_replaces_lru);

	// Prefetch issue
	registerStatsMetric("dp_tlb", core_id, "predictions_issued",           &m_stats.predictions_issued);
	registerStatsMetric("dp_tlb", core_id, "predictions_skipped_invalid_vpn",    &m_stats.predictions_skipped_invalid_vpn);
	registerStatsMetric("dp_tlb", core_id, "predictions_skipped_tlb_resident",   &m_stats.predictions_skipped_tlb_resident);
	registerStatsMetric("dp_tlb", core_id, "predictions_skipped_pq_duplicate", &m_stats.predictions_skipped_pq_duplicate);
	registerStatsMetric("dp_tlb", core_id, "prefetch_attempts",            &m_stats.prefetch_attempts);
	registerStatsMetric("dp_tlb", core_id, "prefetch_successful",          &m_stats.prefetch_successful);
	registerStatsMetric("dp_tlb", core_id, "prefetch_failed",              &m_stats.prefetch_failed);

	// Chained-depth walk (max_depth > 1)
	registerStatsMetric("dp_tlb", core_id, "chain_lookups",                &m_stats.chain_lookups);
	registerStatsMetric("dp_tlb", core_id, "chain_row_hits",               &m_stats.chain_row_hits);
	registerStatsMetric("dp_tlb", core_id, "chain_breaks_no_row",          &m_stats.chain_breaks_no_row);
	registerStatsMetric("dp_tlb", core_id, "chain_breaks_no_slot",         &m_stats.chain_breaks_no_slot);
	registerStatsMetric("dp_tlb", core_id, "chain_breaks_invalid_vpn",     &m_stats.chain_breaks_invalid_vpn);
	registerStatsMetric("dp_tlb", core_id, "chain_predictions_issued",     &m_stats.chain_predictions_issued);
}

// ═══════════════════════════════════════════════════════════════════
//  Constructor / Destructor
// ═══════════════════════════════════════════════════════════════════

DistanceTLBPrefetcher::DistanceTLBPrefetcher(
	Core *_core, MemoryManagerBase *_memory_manager,
	ShmemPerfModel *_shmem_perf_model, String name,
	uint32_t page_shift, uint32_t num_rows,
	uint32_t num_slots, uint32_t assoc,
	bool model_prefetch_walks,
	uint32_t max_depth)
	: TLBPrefetcherBase(_core, _memory_manager, _shmem_perf_model, name),
	  m_page_shift(page_shift),
	  m_num_rows(num_rows),
	  m_num_slots(num_slots),
	  m_assoc(assoc),
	  m_num_sets(num_rows / assoc),
	  m_have_last_miss(false),
	  m_last_miss_vpn(0),
	  m_have_last_distance(false),
	  m_last_distance(0),
	  m_model_prefetch_walks(model_prefetch_walks),
	  m_max_depth(max_depth),
	  m_stats{}
{
	assert(m_assoc > 0 && "assoc must be > 0");
	assert(m_num_rows > 0 && "num_rows must be > 0");
	assert(m_num_slots > 0 && "num_slots must be > 0");
	assert(m_num_rows % m_assoc == 0 && "num_rows must be divisible by assoc");
	assert(m_max_depth >= 1 && "max_depth must be >= 1");

	// Allocate table: m_num_sets * m_assoc rows, each with m_num_slots slots
	m_table.resize(m_num_sets * m_assoc);
	for (auto &row : m_table)
	{
		row.valid = false;
		row.distance_tag = 0;
		row.row_lru = 0;
		row.slots.resize(m_num_slots);
	}

	registerAllStats(_core->getId());
}

DistanceTLBPrefetcher::~DistanceTLBPrefetcher() {}

// ═══════════════════════════════════════════════════════════════════
//  Hash function for distances
// ═══════════════════════════════════════════════════════════════════

uint32_t DistanceTLBPrefetcher::hashDistance(int64_t d) const
{
	// Fold the signed distance into a positive index.
	// XOR the upper and lower 32 bits for decent spread.
	uint64_t u = static_cast<uint64_t>(d);
	uint32_t lo = static_cast<uint32_t>(u);
	uint32_t hi = static_cast<uint32_t>(u >> 32);
	return lo ^ hi;
}

// ═══════════════════════════════════════════════════════════════════
//  Table lookup  (set-associative; assoc=1 → direct-mapped)
// ═══════════════════════════════════════════════════════════════════

bool DistanceTLBPrefetcher::lookupRow(int64_t distance, DPRow *&row)
{
	uint32_t set_idx = hashDistance(distance) % m_num_sets;
	uint32_t base = set_idx * m_assoc;

	for (uint32_t w = 0; w < m_assoc; ++w)
	{
		DPRow &r = m_table[base + w];
		if (r.valid && r.distance_tag == distance)
		{
			// Touch row LRU: set this way to 0, age others in the set.
			for (uint32_t v = 0; v < m_assoc; ++v)
			{
				if (v == w)
					m_table[base + v].row_lru = 0;
				else if (m_table[base + v].valid && m_table[base + v].row_lru < UINT8_MAX)
					m_table[base + v].row_lru++;
			}
			row = &r;
			return true;
		}
	}
	row = nullptr;
	return false;
}

// ═══════════════════════════════════════════════════════════════════
//  Row allocation  (evicts LRU-ish way if set is full)
// ═══════════════════════════════════════════════════════════════════

DPRow *DistanceTLBPrefetcher::allocateRow(int64_t distance)
{
	uint32_t set_idx = hashDistance(distance) % m_num_sets;
	uint32_t base = set_idx * m_assoc;

	// Find an invalid way first.
	for (uint32_t w = 0; w < m_assoc; ++w)
	{
		DPRow &r = m_table[base + w];
		if (!r.valid)
		{
			r.valid = true;
			r.distance_tag = distance;
			for (auto &s : r.slots)
			{
				s.valid = false;
				s.predicted_distance = 0;
				s.lru = 0;
			}
			m_stats.row_allocations++;
			return &r;
		}
	}

	// All ways occupied – evict the LRU way (highest row_lru value).
	uint32_t victim_way = 0;
	uint8_t  max_row_lru = 0;
	for (uint32_t w = 0; w < m_assoc; ++w)
	{
		if (m_table[base + w].row_lru > max_row_lru)
		{
			max_row_lru = m_table[base + w].row_lru;
			victim_way = w;
		}
	}
	DPRow &victim = m_table[base + victim_way];
	victim.valid = true;
	victim.distance_tag = distance;
	victim.row_lru = 0;
	for (auto &s : victim.slots)
	{
		s.valid = false;
		s.predicted_distance = 0;
		s.lru = 0;
	}
	// Age the other ways in the set.
	for (uint32_t v = 0; v < m_assoc; ++v)
	{
		if (v != victim_way && m_table[base + v].valid && m_table[base + v].row_lru < UINT8_MAX)
			m_table[base + v].row_lru++;
	}
	m_stats.row_allocations++;
	m_stats.row_replacements++;
	return &victim;
}

// ═══════════════════════════════════════════════════════════════════
//  LRU touch helper
// ═══════════════════════════════════════════════════════════════════
//
// Sets the touched slot's lru to 0 (most recent) and ages the others.

void DistanceTLBPrefetcher::touchSlotLRU(DPRow &row, uint32_t slot_idx)
{
	for (uint32_t i = 0; i < static_cast<uint32_t>(row.slots.size()); ++i)
	{
		if (i == slot_idx)
			row.slots[i].lru = 0;
		else if (row.slots[i].valid && row.slots[i].lru < UINT8_MAX)
			row.slots[i].lru++;
	}
}

// ═══════════════════════════════════════════════════════════════════
//  Successor distance update
// ═══════════════════════════════════════════════════════════════════
//
// Inserts current_distance into the row keyed by previous_distance.
// Slots are maintained in LRU order.

void DistanceTLBPrefetcher::updateSuccessorDistance(
	int64_t previous_distance, int64_t current_distance)
{
	m_stats.successor_updates++;

	// Find or allocate the row for previous_distance.
	DPRow *row = nullptr;
	if (!lookupRow(previous_distance, row))
		row = allocateRow(previous_distance);

	// Search for current_distance already present.
	for (uint32_t i = 0; i < m_num_slots; ++i)
	{
		if (row->slots[i].valid &&
			row->slots[i].predicted_distance == current_distance)
		{
			// Already stored – refresh LRU.
			touchSlotLRU(*row, i);
			m_stats.successor_hits_existing++;
			return;
		}
	}

	// Not present – try to find a free slot.
	for (uint32_t i = 0; i < m_num_slots; ++i)
	{
		if (!row->slots[i].valid)
		{
			row->slots[i].valid = true;
			row->slots[i].predicted_distance = current_distance;
			touchSlotLRU(*row, i);
			m_stats.successor_inserts_free++;
			return;
		}
	}

	// All slots occupied – evict LRU (highest lru value).
	uint32_t lru_idx = 0;
	uint8_t  max_lru = 0;
	for (uint32_t i = 0; i < m_num_slots; ++i)
	{
		if (row->slots[i].lru > max_lru)
		{
			max_lru = row->slots[i].lru;
			lru_idx = i;
		}
	}
	row->slots[lru_idx].predicted_distance = current_distance;
	touchSlotLRU(*row, lru_idx);
	m_stats.successor_replaces_lru++;
}

// ═══════════════════════════════════════════════════════════════════
//  Direct page-table lookup (no timing)
// ═══════════════════════════════════════════════════════════════════

bool DistanceTLBPrefetcher::directPageTableLookupVPN(
	PageTable *pt, uint64_t vpn,
	uint64_t &ppn, uint32_t &page_size) const
{
	if (!pt) return false;
	IntPtr addr = static_cast<IntPtr>(vpn) << m_page_shift;
	PTWResult r = pt->initializeWalk(addr, /*count*/ false,
									 /*is_prefetch*/ true,
									 /*restart_walk*/ true);
	if (r.fault_happened || r.ppn == 0)
		return false;
	ppn       = static_cast<uint64_t>(r.ppn);
	page_size = static_cast<uint32_t>(r.page_size);
	return true;
}

// ═══════════════════════════════════════════════════════════════════
//  TLB residency check
// ═══════════════════════════════════════════════════════════════════

bool DistanceTLBPrefetcher::inAnyTLB(uint64_t vpn) const
{
	if (m_tlb_hierarchy.empty()) return false;
	IntPtr page_addr = static_cast<IntPtr>(vpn) << m_page_shift;
	for (auto *tlb : m_tlb_hierarchy)
	{
		if (tlb->contains(page_addr, m_page_shift))
			return true;
	}
	return false;
}

// ═══════════════════════════════════════════════════════════════════
//  Issue predictions from a matched row
// ═══════════════════════════════════════════════════════════════════

void DistanceTLBPrefetcher::issuePredictionsFromRow(
	const DPRow &row, uint64_t current_vpn,
	IntPtr eip, Core::lock_signal_t lock,
	bool modeled, bool count, PageTable *pt,
	std::vector<query_entry> &result)
{
	// Build sorted index: most recently used slot first (lowest lru).
	std::vector<uint32_t> order;
	order.reserve(m_num_slots);
	for (uint32_t i = 0; i < m_num_slots; ++i)
	{
		if (row.slots[i].valid)
			order.push_back(i);
	}
	std::sort(order.begin(), order.end(),
		[&](uint32_t a, uint32_t b) { return row.slots[a].lru < row.slots[b].lru; });

	for (uint32_t idx : order)
	{
		const DPPredSlot &slot = row.slots[idx];

		int64_t pred_vpn_signed = static_cast<int64_t>(current_vpn) +
								  slot.predicted_distance;
		if (pred_vpn_signed < 0)
		{
			m_stats.predictions_skipped_invalid_vpn++;
			continue;
		}
		uint64_t predicted_vpn = static_cast<uint64_t>(pred_vpn_signed);

		if (inAnyTLB(predicted_vpn))
		{
			m_stats.predictions_skipped_tlb_resident++;
			continue;
		}

		// PQ duplicate suppression.
		if (m_recently_predicted.count(predicted_vpn))
		{
			m_stats.predictions_skipped_pq_duplicate++;
			continue;
		}

		m_stats.prefetch_attempts++;

		if (m_model_prefetch_walks)
		{
			IntPtr pref_addr = static_cast<IntPtr>(predicted_vpn) << m_page_shift;
			query_entry q = PTWTransparent(pref_addr, eip, lock, modeled, count, pt);
			if (q.ppn == 0)
			{
				m_stats.prefetch_failed++;
				continue;
			}
			result.push_back(q);
		}
		else
		{
			uint64_t ppn = 0;
			uint32_t page_size = 0;
			if (!directPageTableLookupVPN(pt, predicted_vpn, ppn, page_size))
			{
				m_stats.prefetch_failed++;
				continue;
			}
			SubsecondTime now = shmem_perf_model->getElapsedTime(
				ShmemPerfModel::_USER_THREAD);
			query_entry q{};
			q.address      = static_cast<IntPtr>(predicted_vpn) << m_page_shift;
			q.ppn          = static_cast<IntPtr>(ppn);
			q.page_size    = static_cast<int>(page_size);
			q.timestamp    = now;
			q.payload_bits = 0;
			result.push_back(q);
		}

		m_stats.prefetch_successful++;
		m_stats.predictions_issued++;

		// Track for PQ duplicate suppression.
		if (m_recently_predicted.size() >= MAX_RECENTLY_PREDICTED)
			m_recently_predicted.clear();
		m_recently_predicted.insert(predicted_vpn);
	}
}

// ═══════════════════════════════════════════════════════════════════
//  performPrefetch – main entry point
// ═══════════════════════════════════════════════════════════════════

std::vector<query_entry> DistanceTLBPrefetcher::performPrefetch(
	IntPtr address, IntPtr eip, Core::lock_signal_t lock,
	bool modeled, bool count, PageTable *pt,
	bool instruction, bool tlb_hit, bool pq_hit, int page_size)
{
	std::vector<query_entry> result;
	if (!pt) return result;

	uint64_t vpn = static_cast<uint64_t>(address) >> m_page_shift;

	m_stats.queries++;
	if (instruction)
		m_stats.queries_instruction++;
	else
		m_stats.queries_data++;

	// DP only observes the TLB miss stream.
	if (tlb_hit)
		return result;

	m_stats.tlb_misses++;

	// ── First miss: initialize history, nothing to predict yet ───
	if (!m_have_last_miss)
	{
		m_have_last_miss = true;
		m_last_miss_vpn = vpn;
		m_stats.first_miss_no_distance++;
		return result;
	}

	// ── Compute current distance ─────────────────────────────────
	int64_t current_distance = static_cast<int64_t>(vpn) -
							   static_cast<int64_t>(m_last_miss_vpn);

	m_stats.distance_computed++;
	if (current_distance == 0)
		m_stats.zero_distance_seen++;
	else if (current_distance > 0)
		m_stats.positive_distance_seen++;
	else
		m_stats.negative_distance_seen++;

	// ── 1) Prediction: look up table by current_distance ────────
	m_stats.table_lookups++;
	DPRow *hit_row = nullptr;
	if (lookupRow(current_distance, hit_row))
	{
		m_stats.table_hits++;
		issuePredictionsFromRow(*hit_row, vpn, eip, lock,
								modeled, count, pt, result);

		// ── 1b) Chained Markov walk (max_depth > 1) ──────────────
		// Follow the MRU successor distance: advance the base VPN by
		// it, re-key the table with it, and issue that row's
		// predictions from the advanced VPN.  Each level is one more
		// speculative step ahead of the demand miss stream.
		DPRow   *cur_row  = hit_row;
		uint64_t base_vpn = vpn;
		for (uint32_t depth = 2; depth <= m_max_depth; ++depth)
		{
			// MRU valid slot of the current row (lowest lru value).
			const DPPredSlot *mru = nullptr;
			for (uint32_t i = 0; i < m_num_slots; ++i)
			{
				const DPPredSlot &s = cur_row->slots[i];
				if (s.valid && (!mru || s.lru < mru->lru))
					mru = &s;
			}
			if (!mru)
			{
				m_stats.chain_breaks_no_slot++;
				break;
			}

			int64_t next_dist = mru->predicted_distance;
			int64_t next_vpn_signed = static_cast<int64_t>(base_vpn) + next_dist;
			if (next_vpn_signed < 0)
			{
				m_stats.chain_breaks_invalid_vpn++;
				break;
			}
			base_vpn = static_cast<uint64_t>(next_vpn_signed);

			m_stats.chain_lookups++;
			DPRow *next_row = nullptr;
			if (!lookupRow(next_dist, next_row))
			{
				m_stats.chain_breaks_no_row++;
				break;
			}
			m_stats.chain_row_hits++;

			UInt64 issued_before = m_stats.predictions_issued;
			issuePredictionsFromRow(*next_row, base_vpn, eip, lock,
									modeled, count, pt, result);
			m_stats.chain_predictions_issued +=
				m_stats.predictions_issued - issued_before;

			cur_row = next_row;
		}
	}
	else
	{
		m_stats.table_misses++;
		// Allocate a row so that future updates can populate it.
		allocateRow(current_distance);
	}

	// ── 2) Learn: insert current_distance as successor of
	//       previous_distance ────────────────────────────────────
	if (m_have_last_distance)
		updateSuccessorDistance(m_last_distance, current_distance);

	// ── 3) Advance history ───────────────────────────────────────
	m_last_distance = current_distance;
	m_have_last_distance = true;
	m_last_miss_vpn = vpn;

	return result;
}

} // namespace ParametricDramDirectoryMSI
