// RecencyTLBPrefetcher.cc – Recency-based TLB preloading
//
// Maintains a doubly-linked recency list of translations that have been
// evicted from the TLB.  On a TLB miss to page Y the prefetcher reads
// Y's neighbors in the list (same-recency, recency-1, recency+1) and
// returns them as query_entry objects so the simulator inserts them into
// its existing prefetch queue (PQ).  No private prefetch buffer is used.

#include "RecencyTLBPrefetcher.h"
#include "mmu_base.h"
#include "base_filter.h"
#include "tlb.h"
#include "stats.h"

namespace ParametricDramDirectoryMSI
{

// ═══════════════════════════════════════════════════════════════════
//  Stats registration
// ═══════════════════════════════════════════════════════════════════

void RecencyTLBPrefetcher::registerAllStats(core_id_t core_id)
{
	// Core
	registerStatsMetric("recency_tlb", core_id, "queries",                    &m_stats.queries);
	registerStatsMetric("recency_tlb", core_id, "queries_instruction",        &m_stats.queries_instruction);
	registerStatsMetric("recency_tlb", core_id, "queries_data",              &m_stats.queries_data);
	registerStatsMetric("recency_tlb", core_id, "tlb_hits",                  &m_stats.tlb_hits);
	registerStatsMetric("recency_tlb", core_id, "tlb_misses",               &m_stats.tlb_misses);

	// PQ interaction
	registerStatsMetric("recency_tlb", core_id, "pq_hits_on_demand_miss",    &m_stats.pq_hits_on_demand_miss);
	registerStatsMetric("recency_tlb", core_id, "pq_misses_on_demand_miss",  &m_stats.pq_misses_on_demand_miss);
	registerStatsMetric("recency_tlb", core_id, "predictions_returned_to_pq",&m_stats.predictions_returned_to_pq);

	// Prediction
	registerStatsMetric("recency_tlb", core_id, "predictions_same_issued",   &m_stats.predictions_same_issued);
	registerStatsMetric("recency_tlb", core_id, "predictions_minus1_issued", &m_stats.predictions_minus1_issued);
	registerStatsMetric("recency_tlb", core_id, "predictions_plus1_issued",  &m_stats.predictions_plus1_issued);
	registerStatsMetric("recency_tlb", core_id, "predictions_same_failed",   &m_stats.predictions_same_failed);
	registerStatsMetric("recency_tlb", core_id, "predictions_minus1_failed", &m_stats.predictions_minus1_failed);
	registerStatsMetric("recency_tlb", core_id, "predictions_plus1_failed",  &m_stats.predictions_plus1_failed);
	registerStatsMetric("recency_tlb", core_id, "predictions_skipped_invalid",       &m_stats.predictions_skipped_invalid);
	registerStatsMetric("recency_tlb", core_id, "predictions_skipped_tlb_resident",  &m_stats.predictions_skipped_tlb_resident);
	registerStatsMetric("recency_tlb", core_id, "predictions_skipped_pq_duplicate", &m_stats.predictions_skipped_pq_duplicate);

	// Recency structure
	registerStatsMetric("recency_tlb", core_id, "recency_unhooks",           &m_stats.recency_unhooks);
	registerStatsMetric("recency_tlb", core_id, "recency_pushes",            &m_stats.recency_pushes);
	registerStatsMetric("recency_tlb", core_id, "recency_nodes_created",     &m_stats.recency_nodes_created);
	registerStatsMetric("recency_tlb", core_id, "recency_missing_node",      &m_stats.recency_missing_node);
	registerStatsMetric("recency_tlb", core_id, "recency_victim_insertions", &m_stats.recency_victim_insertions);

	// Timing
	registerStatsMetric("recency_tlb", core_id, "prefetch_attempts",         &m_stats.prefetch_attempts);
	registerStatsMetric("recency_tlb", core_id, "prefetch_successful",       &m_stats.prefetch_successful);
	registerStatsMetric("recency_tlb", core_id, "prefetch_failed",           &m_stats.prefetch_failed);

	// Pointer-chase cache access breakdown
	registerStatsMetric("recency_tlb", core_id, "pointer_chase_accesses",    &m_stats.pointer_chase_accesses);
	registerStatsMetric("recency_tlb", core_id, "pointer_chase_pwc_hits",    &m_stats.pointer_chase_pwc_hits);
	registerStatsMetric("recency_tlb", core_id, "pointer_chase_l1_hits",     &m_stats.pointer_chase_l1_hits);
	registerStatsMetric("recency_tlb", core_id, "pointer_chase_l2_hits",     &m_stats.pointer_chase_l2_hits);
	registerStatsMetric("recency_tlb", core_id, "pointer_chase_llc_hits",    &m_stats.pointer_chase_llc_hits);
	registerStatsMetric("recency_tlb", core_id, "pointer_chase_dram_hits",   &m_stats.pointer_chase_dram_hits);
}

// ═══════════════════════════════════════════════════════════════════
//  Constructor / Destructor
// ═══════════════════════════════════════════════════════════════════

RecencyTLBPrefetcher::RecencyTLBPrefetcher(
	Core *_core, MemoryManagerBase *_memory_manager,
	ShmemPerfModel *_shmem_perf_model, String name,
	uint32_t page_shift,
	bool prefetch_same_recency,
	bool prefetch_recency_minus_1,
	bool prefetch_recency_plus_1,
	bool prefetch_on_tlb_hit,
	bool model_prefetch_walks,
	bool consume_pq_on_hit,
	bool model_pointer_chase)
	: TLBPrefetcherBase(_core, _memory_manager, _shmem_perf_model, name),
	  m_stack_head_vpn(RECENCY_INVALID_VPN),
	  m_stack_tail_vpn(RECENCY_INVALID_VPN),
	  m_pending_victim_vpn(RECENCY_INVALID_VPN),
	  m_page_shift(page_shift),
	  m_prefetch_same_recency(prefetch_same_recency),
	  m_prefetch_recency_minus_1(prefetch_recency_minus_1),
	  m_prefetch_recency_plus_1(prefetch_recency_plus_1),
	  m_prefetch_on_tlb_hit(prefetch_on_tlb_hit),
	  m_model_prefetch_walks(model_prefetch_walks),
	  m_consume_pq_on_hit(consume_pq_on_hit),
	  m_model_pointer_chase(model_pointer_chase),
	  m_pointer_table(
		// Physical base: 0xE000'0000'0000 + core_id * 256 MB
		// Chosen high enough to avoid overlap with real application physical memory.
		0xE00000000000ULL + static_cast<uint64_t>(_core->getId()) * (256ULL << 20)),
	  m_stats{}
{
	registerAllStats(_core->getId());
}

RecencyTLBPrefetcher::~RecencyTLBPrefetcher() {}

// ═══════════════════════════════════════════════════════════════════
//  Direct page-table lookup (no timing, no stats)
// ═══════════════════════════════════════════════════════════════════

bool RecencyTLBPrefetcher::directPageTableLookupVPN(
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
//  Node lookup / lazy creation
// ═══════════════════════════════════════════════════════════════════

RecencyNode *RecencyTLBPrefetcher::getNode(uint64_t vpn)
{
	auto it = m_nodes.find(vpn);
	if (it == m_nodes.end()) return nullptr;
	return &it->second;
}

RecencyNode *RecencyTLBPrefetcher::getOrCreateNode(PageTable *pt, uint64_t vpn)
{
	auto it = m_nodes.find(vpn);
	if (it != m_nodes.end())
		return &it->second;

	// Lazy creation – validate mapping via direct PT lookup.
	uint64_t ppn = 0;
	uint32_t page_size = 0;
	if (!directPageTableLookupVPN(pt, vpn, ppn, page_size))
		return nullptr;

	RecencyNode &node = m_nodes[vpn];
	node.valid    = true;
	node.vpn      = vpn;
	node.ppn      = ppn;
	node.prev_vpn = RECENCY_INVALID_VPN;
	node.next_vpn = RECENCY_INVALID_VPN;
	node.in_tlb   = false;
	m_pointer_table.ensureAllocated(vpn);
	m_stats.recency_nodes_created++;
	return &node;
}

// ═══════════════════════════════════════════════════════════════════
//  Doubly-linked list helpers  (memory-resident recency stack)
// ═══════════════════════════════════════════════════════════════════

void RecencyTLBPrefetcher::listRemove(uint64_t vpn)
{
	RecencyNode *n = getNode(vpn);
	if (!n) return;

	uint64_t p = n->prev_vpn;
	uint64_t nx = n->next_vpn;

	if (p != RECENCY_INVALID_VPN)
	{
		RecencyNode *pn = getNode(p);
		if (pn) pn->next_vpn = nx;
	}
	else
	{
		// n was the head
		m_stack_head_vpn = nx;
	}

	if (nx != RECENCY_INVALID_VPN)
	{
		RecencyNode *nn = getNode(nx);
		if (nn) nn->prev_vpn = p;
	}
	else
	{
		// n was the tail
		m_stack_tail_vpn = p;
	}

	n->prev_vpn = RECENCY_INVALID_VPN;
	n->next_vpn = RECENCY_INVALID_VPN;
	m_stats.recency_unhooks++;
}

void RecencyTLBPrefetcher::listPushFront(uint64_t vpn)
{
	RecencyNode *n = getNode(vpn);
	if (!n) return;

	// Ensure radix pages exist for this VPN's pointer entry
	m_pointer_table.ensureAllocated(vpn);

	n->prev_vpn = RECENCY_INVALID_VPN;
	n->next_vpn = m_stack_head_vpn;

	if (m_stack_head_vpn != RECENCY_INVALID_VPN)
	{
		RecencyNode *old_head = getNode(m_stack_head_vpn);
		if (old_head) old_head->prev_vpn = vpn;
	}

	m_stack_head_vpn = vpn;

	if (m_stack_tail_vpn == RECENCY_INVALID_VPN)
		m_stack_tail_vpn = vpn;

	m_stats.recency_pushes++;
}

bool RecencyTLBPrefetcher::listContains(uint64_t vpn) const
{
	auto it = m_nodes.find(vpn);
	if (it == m_nodes.end()) return false;
	const RecencyNode &n = it->second;
	if (vpn == m_stack_head_vpn) return true;
	if (vpn == m_stack_tail_vpn) return true;
	if (n.prev_vpn != RECENCY_INVALID_VPN) return true;
	return false;
}

// ═══════════════════════════════════════════════════════════════════
//  Neighbor capture (before mutation)
// ═══════════════════════════════════════════════════════════════════

void RecencyTLBPrefetcher::capturePredictionNeighbors(
	uint64_t vpn,
	uint64_t &same_vpn,
	uint64_t &minus1_vpn,
	uint64_t &plus1_vpn)
{
	same_vpn   = RECENCY_INVALID_VPN;
	minus1_vpn = RECENCY_INVALID_VPN;
	plus1_vpn  = RECENCY_INVALID_VPN;

	RecencyNode *y = getNode(vpn);
	if (!y) return;

	// Same recency = Y.prev  (the entry immediately above Y in the stack)
	if (y->prev_vpn != RECENCY_INVALID_VPN)
	{
		same_vpn = y->prev_vpn;
		// Recency-1 = same.prev (one position further above)
		RecencyNode *x = getNode(same_vpn);
		if (x && x->prev_vpn != RECENCY_INVALID_VPN)
			minus1_vpn = x->prev_vpn;
	}

	// Recency+1 = Y.next
	if (y->next_vpn != RECENCY_INVALID_VPN)
		plus1_vpn = y->next_vpn;
}

// ═══════════════════════════════════════════════════════════════════
//  Victim consumption (called BEFORE neighbor capture)
//
//  Pushes the pending victim V(N-1) to the list head.  Called at the
//  start of performPrefetch(N) so that V(N-1) is present in the
//  recency list when we capture neighbors for the demanded page.
// ═══════════════════════════════════════════════════════════════════

void RecencyTLBPrefetcher::consumePendingVictim()
{
	if (m_pending_victim_vpn == RECENCY_INVALID_VPN)
		return;

	RecencyNode *victim = getNode(m_pending_victim_vpn);
	if (victim)
	{
		victim->in_tlb = false;
		if (listContains(m_pending_victim_vpn))
			listRemove(m_pending_victim_vpn);
		listPushFront(m_pending_victim_vpn);
		m_stats.recency_victim_insertions++;
	}
	m_pending_victim_vpn = RECENCY_INVALID_VPN;
}

// ═══════════════════════════════════════════════════════════════════
//  Recency update on TLB fill (called AFTER neighbor capture)
//
//  Removes the demanded page from the memory-resident list — it is
//  now in the TLB portion of the logical recency stack.
// ═══════════════════════════════════════════════════════════════════

void RecencyTLBPrefetcher::onTranslationInstalledIntoTLB(uint64_t demanded_vpn)
{
	if (listContains(demanded_vpn))
		listRemove(demanded_vpn);

	RecencyNode *n = getNode(demanded_vpn);
	if (n)
		n->in_tlb = true;
}

// ═══════════════════════════════════════════════════════════════════
//  TLB residency check
// ═══════════════════════════════════════════════════════════════════

bool RecencyTLBPrefetcher::inAnyTLB(uint64_t vpn) const
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
//  Pointer-chase latency through cache hierarchy
//
//  The recency list prev/next pointers are stored in a separate
//  per-app radix page table (RecencyPointerTable).  Each pointer
//  chase walks 4 radix levels; each level produces a cache access
//  at the page's physical address.  The total latency is the sum
//  of all level accesses (serialised — each level depends on the
//  previous one's result).
// ═══════════════════════════════════════════════════════════════════

SubsecondTime RecencyTLBPrefetcher::modelPointerChase(
	uint64_t target_vpn, PageTable *pt,
	IntPtr eip, Core::lock_signal_t lock,
	bool modeled, bool count)
{
	if (!m_model_pointer_chase)
		return SubsecondTime::Zero();

	auto *mmu_base = static_cast<ParametricDramDirectoryMSI::MemoryManagementUnitBase *>(
		memory_manager->getMMU());

	// Get the physical addresses for each level of the radix walk.
	std::vector<IntPtr> walk_addrs;
	m_pointer_table.getWalkAddresses(target_vpn, walk_addrs);

	// Reuse the existing Page Walk Cache (PWC) for the first 3 directory
	// levels of the pointer table.  The PWC is a small dedicated cache
	// that stores intermediate page-table entries.  Since the pointer
	// table has the same radix structure, its directory entries benefit
	// from PWC caching just like regular page table directory entries.
	// Only the leaf level (index 3, the actual prev/next pointers) must
	// always go through the full cache hierarchy.
	BaseFilter *pwc_filter = mmu_base->getPTWFilter();

	SubsecondTime total_latency = SubsecondTime::Zero();
	SubsecondTime t_walk = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);

	for (size_t i = 0; i < walk_addrs.size(); i++)
	{
		IntPtr phys_addr = walk_addrs[i];
		if (phys_addr == 0)
			continue;  // level not allocated → skip (treated as fast)

		// Directory levels (i=0,1,2) map to PWC levels 0,1,2.
		// If the PWC hits, the directory entry is already cached and
		// we skip the expensive cache-hierarchy access for this level.
		if (i < 3 && pwc_filter && pwc_filter->lookupPWC(phys_addr, t_walk, static_cast<int>(i), count))
		{
			m_stats.pointer_chase_accesses++;
			m_stats.pointer_chase_pwc_hits++;
			continue;
		}

		MemoryManagementUnitBase::translationPacket packet(
			phys_addr, eip, /*instruction=*/false, lock,
			modeled, count,
			CacheBlockInfo::block_type_t::PAGE_TABLE_DATA);

		HitWhere::where_t hit_where = HitWhere::UNKNOWN;
		SubsecondTime level_latency = mmu_base->accessCache(
			packet, t_walk, /*is_prefetch=*/false, hit_where);

		// Serialise: next level access starts after this one completes
		t_walk += level_latency;
		total_latency += level_latency;

		// Track cache-level breakdown
		m_stats.pointer_chase_accesses++;
		switch (hit_where)
		{
		case HitWhere::L1_OWN:
			m_stats.pointer_chase_l1_hits++;
			break;
		case HitWhere::L2_OWN:
			m_stats.pointer_chase_l2_hits++;
			break;
		case HitWhere::NUCA_CACHE:
			m_stats.pointer_chase_llc_hits++;
			break;
		default:
			m_stats.pointer_chase_dram_hits++;
			break;
		}
	}

	return total_latency;
}

// ═══════════════════════════════════════════════════════════════════
//  Victim notification from TLB evictions
// ═══════════════════════════════════════════════════════════════════

void RecencyTLBPrefetcher::notifyVictim(IntPtr victim_address, int page_size, IntPtr ppn)
{
	uint64_t vpn = static_cast<uint64_t>(victim_address) >> m_page_shift;

	// Ensure a node exists for the victim (it may already be tracked).
	auto it = m_nodes.find(vpn);
	if (it == m_nodes.end())
	{
		RecencyNode &node = m_nodes[vpn];
		node.valid    = true;
		node.vpn      = vpn;
		node.ppn      = static_cast<uint64_t>(ppn);
		node.prev_vpn = RECENCY_INVALID_VPN;
		node.next_vpn = RECENCY_INVALID_VPN;
		node.in_tlb   = false;
		m_pointer_table.ensureAllocated(vpn);
		m_stats.recency_nodes_created++;
	}
	else
	{
		it->second.ppn = static_cast<uint64_t>(ppn);
	}

	// Buffer the victim VPN; it will be consumed by the next
	// onTranslationInstalledIntoTLB() call and pushed to the list head.
	m_pending_victim_vpn = vpn;
}

// ═══════════════════════════════════════════════════════════════════
//  Query-entry construction
// ═══════════════════════════════════════════════════════════════════

query_entry RecencyTLBPrefetcher::makeQueryEntry(
	uint64_t vpn, uint64_t ppn,
	uint32_t page_size, SubsecondTime ts) const
{
	query_entry q{};
	q.address      = static_cast<IntPtr>(vpn) << m_page_shift;
	q.ppn          = static_cast<IntPtr>(ppn);
	q.page_size    = static_cast<int>(page_size);
	q.timestamp    = ts;
	q.payload_bits = 0;
	return q;
}

// ═══════════════════════════════════════════════════════════════════
//  Issue a single prediction candidate
// ═══════════════════════════════════════════════════════════════════

void RecencyTLBPrefetcher::issuePredictionCandidate(
	uint64_t candidate_vpn,
	IntPtr eip,
	Core::lock_signal_t lock,
	bool modeled, bool count,
	PageTable *pt,
	std::vector<query_entry> &result,
	PredictionKind kind,
	uint32_t extra_pointer_chases)
{
	if (candidate_vpn == RECENCY_INVALID_VPN)
	{
		m_stats.predictions_skipped_invalid++;
		return;
	}

	if (inAnyTLB(candidate_vpn))
	{
		m_stats.predictions_skipped_tlb_resident++;
		return;
	}

	// Per-VPN PQ duplicate suppression: avoid re-predicting a VPN that
	// was recently issued and is likely still in the prefetch queue.
	if (m_recently_predicted.count(candidate_vpn))
	{
		m_stats.predictions_skipped_pq_duplicate++;
		return;
	}

	m_stats.prefetch_attempts++;

	if (m_model_prefetch_walks)
	{
		// Use the base-class transparent PTW for a modeled walk.
		IntPtr pref_addr = static_cast<IntPtr>(candidate_vpn) << m_page_shift;
		query_entry q = PTWTransparent(pref_addr, eip, lock, modeled, count, pt);
		if (q.ppn == 0)
		{
			m_stats.prefetch_failed++;
			switch (kind)
			{
			case PRED_SAME:   m_stats.predictions_same_failed++;   break;
			case PRED_MINUS1: m_stats.predictions_minus1_failed++; break;
			case PRED_PLUS1:  m_stats.predictions_plus1_failed++;  break;
			}
			return;
		}
		result.push_back(q);
	}
	else
	{
		// Direct (unmodeled) lookup – cheaper for v1.
		uint64_t ppn = 0;
		uint32_t page_size = 0;
		if (!directPageTableLookupVPN(pt, candidate_vpn, ppn, page_size))
		{
			m_stats.prefetch_failed++;
			switch (kind)
			{
			case PRED_SAME:   m_stats.predictions_same_failed++;   break;
			case PRED_MINUS1: m_stats.predictions_minus1_failed++; break;
			case PRED_PLUS1:  m_stats.predictions_plus1_failed++;  break;
			}
			return;
		}
		SubsecondTime now = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);
		result.push_back(makeQueryEntry(candidate_vpn, ppn, page_size, now));
	}

	// Charge pointer-chase latency for in-memory list traversal.
	// The recency list prev/next pointers live in PTEs.  PTE(Y) is
	// fetched by the demand PTW (free), but reading PTE(same) for minus1
	// requires an additional cache access.  We issue each pointer chase
	// through the cache hierarchy so latency reflects actual cache warmth.
	if (extra_pointer_chases > 0 && m_model_pointer_chase)
	{
		SubsecondTime chase_cost = SubsecondTime::Zero();
		for (uint32_t i = 0; i < extra_pointer_chases; i++)
			chase_cost += modelPointerChase(candidate_vpn, pt, eip, lock, modeled, count);
		result.back().timestamp += chase_cost;
	}

	m_stats.prefetch_successful++;
	m_stats.predictions_returned_to_pq++;

	// Track predicted VPNs for PQ duplicate suppression.
	if (m_recently_predicted.size() >= MAX_RECENTLY_PREDICTED)
		m_recently_predicted.clear();
	m_recently_predicted.insert(candidate_vpn);

	switch (kind)
	{
	case PRED_SAME:   m_stats.predictions_same_issued++;   break;
	case PRED_MINUS1: m_stats.predictions_minus1_issued++; break;
	case PRED_PLUS1:  m_stats.predictions_plus1_issued++;  break;
	}
}

// ═══════════════════════════════════════════════════════════════════
//  performPrefetch – main entry point
// ═══════════════════════════════════════════════════════════════════

std::vector<query_entry> RecencyTLBPrefetcher::performPrefetch(
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

	// ── Step 2: ignore plain TLB hits by default ─────────────────
	if (tlb_hit && !m_prefetch_on_tlb_hit)
	{
		m_stats.tlb_hits++;
		return result;
	}

	if (!tlb_hit)
		m_stats.tlb_misses++;

	// ── Step 3: PQ interaction tracking ──────────────────────────
	if (!tlb_hit)
	{
		if (pq_hit)
			m_stats.pq_hits_on_demand_miss++;
		else
			m_stats.pq_misses_on_demand_miss++;
	}

	// ── Ensure a node exists for the demanded translation ────────
	RecencyNode *node = getOrCreateNode(pt, vpn);
	if (!node)
	{
		m_stats.recency_missing_node++;
		return result;
	}

	// ── Step 1: consume the pending victim from the prior miss ───
	// The victim V(N-1) must be pushed to the list head BEFORE we
	// capture neighbors, so that V(N-1) is available as a neighbor
	// of the demanded page.  Event ordering:
	//   Phase 3 of miss N-1: TLB::allocate() evicts V(N-1) →
	//       notifyVictim() buffers V(N-1)
	//   Phase 1 of miss  N : performPrefetch(N) runs →
	//       consumePendingVictim() pushes V(N-1) to head
	consumePendingVictim();

	// ── Step 2: capture predictors BEFORE mutation ───────────────
	uint64_t same_vpn, minus1_vpn, plus1_vpn;
	capturePredictionNeighbors(vpn, same_vpn, minus1_vpn, plus1_vpn);

	// ── Step 3: remove demanded page from the memory-resident list
	onTranslationInstalledIntoTLB(vpn);

	// If the list was empty before we updated it (cold start), there
	// are no meaningful neighbors to predict with.
	if (same_vpn == RECENCY_INVALID_VPN &&
		minus1_vpn == RECENCY_INVALID_VPN &&
		plus1_vpn == RECENCY_INVALID_VPN)
	{
		return result;
	}

	// ── Generate speculative prefetches → returned as query_entry
	//    objects so the simulator inserts them into the existing PQ ─
	//
	// Pointer-chase cost model (in-memory recency list):
	//   PTE(Y) is fetched by the demand PTW at no extra cost.
	//   PTE(Y).prev → same_vpn  : 0 extra chases (part of PTE(Y))
	//   PTE(same).prev → minus1 : 1 extra chase  (must fetch PTE(same))
	//   PTE(Y).next → plus1_vpn : 0 extra chases (part of PTE(Y))
	if (m_prefetch_same_recency)
		issuePredictionCandidate(same_vpn, eip, lock, modeled, count, pt,
								result, PRED_SAME, /*extra_pointer_chases=*/0);

	if (m_prefetch_recency_minus_1)
		issuePredictionCandidate(minus1_vpn, eip, lock, modeled, count, pt,
								result, PRED_MINUS1, /*extra_pointer_chases=*/1);

	if (m_prefetch_recency_plus_1)
		issuePredictionCandidate(plus1_vpn, eip, lock, modeled, count, pt,
								result, PRED_PLUS1, /*extra_pointer_chases=*/0);

	return result;
}

} // namespace ParametricDramDirectoryMSI
