// ═══════════════════════════════════════════════════════════════════════════
// AgileTLBPrefetcher.cc – ATP + SBFP implementation
//
// See AgileTLBPrefetcher.h for the full design overview.
// ═══════════════════════════════════════════════════════════════════════════

#include "AgileTLBPrefetcher.h"
#include "mmu_base.h"
#include "../memory_manager.h"
#include "stats.h"
#include "simulator.h"
#include "config.hpp"
#include "tlb.h"
#include <algorithm>
#include <cassert>
#include <cstring>

namespace ParametricDramDirectoryMSI
{

// ═══════════════════════════════════════════════════════════════════
//  Stats registration
// ═══════════════════════════════════════════════════════════════════

void AgileTLBPrefetcher::registerAllStats(core_id_t core_id)
{
	const char *cat = "tlb_atp";

	// ATP decision
	registerStatsMetric(cat, core_id, "queries",              &m_stats.queries);
	registerStatsMetric(cat, core_id, "pq_hits",              &m_stats.pq_hits);
	registerStatsMetric(cat, core_id, "pq_misses",            &m_stats.pq_misses);
	registerStatsMetric(cat, core_id, "sampler_hits",          &m_stats.sampler_hits);
	registerStatsMetric(cat, core_id, "sampler_misses",        &m_stats.sampler_misses);
	registerStatsMetric(cat, core_id, "enable_pref_inc",       &m_stats.enable_pref_inc);
	registerStatsMetric(cat, core_id, "enable_pref_dec",       &m_stats.enable_pref_dec);
	registerStatsMetric(cat, core_id, "select1_inc",           &m_stats.select1_inc);
	registerStatsMetric(cat, core_id, "select1_dec",           &m_stats.select1_dec);
	registerStatsMetric(cat, core_id, "select2_inc",           &m_stats.select2_inc);
	registerStatsMetric(cat, core_id, "select2_dec",           &m_stats.select2_dec);
	registerStatsMetric(cat, core_id, "atp_choice_disable",    &m_stats.atp_choice_disable);
	registerStatsMetric(cat, core_id, "atp_choice_h2p",        &m_stats.atp_choice_h2p);
	registerStatsMetric(cat, core_id, "atp_choice_masp",       &m_stats.atp_choice_masp);
	registerStatsMetric(cat, core_id, "atp_choice_stp",        &m_stats.atp_choice_stp);

	// Real prefetch issue
	registerStatsMetric(cat, core_id, "real_prefetch_candidates_h2p",  &m_stats.real_prefetch_candidates_h2p);
	registerStatsMetric(cat, core_id, "real_prefetch_candidates_masp", &m_stats.real_prefetch_candidates_masp);
	registerStatsMetric(cat, core_id, "real_prefetch_candidates_stp",  &m_stats.real_prefetch_candidates_stp);
	registerStatsMetric(cat, core_id, "real_prefetch_walks_issued",    &m_stats.real_prefetch_walks_issued);
	registerStatsMetric(cat, core_id, "real_prefetch_successful",      &m_stats.real_prefetch_successful);
	registerStatsMetric(cat, core_id, "real_prefetch_failed",          &m_stats.real_prefetch_failed);

	// SBFP
	registerStatsMetric(cat, core_id, "fdt_updates_from_pq",      &m_stats.fdt_updates_from_pq);
	registerStatsMetric(cat, core_id, "fdt_updates_from_sampler",  &m_stats.fdt_updates_from_sampler);
	registerStatsMetric(cat, core_id, "fdt_decay_events",          &m_stats.fdt_decay_events);
	registerStatsMetric(cat, core_id, "free_candidates_seen",      &m_stats.free_candidates_seen);
	registerStatsMetric(cat, core_id, "free_inserted_pq",          &m_stats.free_inserted_pq);
	registerStatsMetric(cat, core_id, "free_inserted_sampler",     &m_stats.free_inserted_sampler);

	// Per-distance stats
	static const char *dist_names[14] = {
		"m7","m6","m5","m4","m3","m2","m1",
		"p1","p2","p3","p4","p5","p6","p7"
	};
	for (uint32_t i = 0; i < 14; i++)
	{
		String seen_name    = String("dist_") + dist_names[i] + "_seen";
		String pq_name      = String("dist_") + dist_names[i] + "_pq";
		String sampler_name = String("dist_") + dist_names[i] + "_sampler";
		String hits_name    = String("dist_") + dist_names[i] + "_hits";
		registerStatsMetric(cat, core_id, seen_name.c_str(),    &m_stats.dist_seen[i]);
		registerStatsMetric(cat, core_id, pq_name.c_str(),      &m_stats.dist_pq[i]);
		registerStatsMetric(cat, core_id, sampler_name.c_str(), &m_stats.dist_sampler[i]);
		registerStatsMetric(cat, core_id, hits_name.c_str(),    &m_stats.dist_hits[i]);
	}

	// FPQ
	registerStatsMetric(cat, core_id, "fpq_p0_hits",            &m_stats.fpq_p0_hits);
	registerStatsMetric(cat, core_id, "fpq_p1_hits",            &m_stats.fpq_p1_hits);
	registerStatsMetric(cat, core_id, "fpq_p2_hits",            &m_stats.fpq_p2_hits);
	registerStatsMetric(cat, core_id, "fpq_p0_inserts",         &m_stats.fpq_p0_inserts);
	registerStatsMetric(cat, core_id, "fpq_p1_inserts",         &m_stats.fpq_p1_inserts);
	registerStatsMetric(cat, core_id, "fpq_p2_inserts",         &m_stats.fpq_p2_inserts);
	registerStatsMetric(cat, core_id, "fpq_fake_base_preds_p0", &m_stats.fpq_fake_base_preds_p0);
	registerStatsMetric(cat, core_id, "fpq_fake_base_preds_p1", &m_stats.fpq_fake_base_preds_p1);
	registerStatsMetric(cat, core_id, "fpq_fake_base_preds_p2", &m_stats.fpq_fake_base_preds_p2);
	registerStatsMetric(cat, core_id, "fpq_fake_free_preds_p0", &m_stats.fpq_fake_free_preds_p0);
	registerStatsMetric(cat, core_id, "fpq_fake_free_preds_p1", &m_stats.fpq_fake_free_preds_p1);
	registerStatsMetric(cat, core_id, "fpq_fake_free_preds_p2", &m_stats.fpq_fake_free_preds_p2);

	// Demand walk
	registerStatsMetric(cat, core_id, "demand_walks",            &m_stats.demand_walks);
	registerStatsMetric(cat, core_id, "demand_walks_successful", &m_stats.demand_walks_successful);
	registerStatsMetric(cat, core_id, "demand_walks_failed",     &m_stats.demand_walks_failed);
}

// ═══════════════════════════════════════════════════════════════════
//  Constructor
// ═══════════════════════════════════════════════════════════════════
//
// All sizing parameters come from the config file (see mmu_atp.cfg
// for the paper's design points).  ATP selection counters start at 0
// (midpoint initialisation is implicit: the MSB check means 0 maps
// to the "off" / "P1-side" of each counter).  FDT counters and
// all FIFO structures start empty.

AgileTLBPrefetcher::AgileTLBPrefetcher(
	Core *_core, MemoryManagerBase *_memory_manager,
	ShmemPerfModel *_shmem_perf_model, String name,
	uint32_t pq_size, uint32_t sampler_size,
	uint32_t fpq_size, uint32_t fdt_counter_bits,
	uint16_t fdt_threshold,
	uint32_t enable_pref_bits, uint32_t select1_bits,
	uint32_t select2_bits,
	uint32_t masp_entries, uint32_t masp_assoc,
	uint32_t page_shift,
	uint32_t masp_lookahead, uint32_t masp_degree)
	: TLBPrefetcherBase(_core, _memory_manager, _shmem_perf_model, name)
	, m_sampler_size(sampler_size)
	, m_fpq_size(fpq_size)
	, m_fdt_counter_bits(fdt_counter_bits)
	, m_fdt_max(static_cast<uint16_t>((1u << fdt_counter_bits) - 1))
	, m_fdt_threshold(fdt_threshold)
	, m_enable_pref_bits(enable_pref_bits)
	, m_select1_bits(select1_bits)
	, m_select2_bits(select2_bits)
	, m_enable_pref_max(static_cast<uint8_t>((1u << enable_pref_bits) - 1))
	, m_select1_max(static_cast<uint8_t>((1u << select1_bits) - 1))
	, m_select2_max(static_cast<uint8_t>((1u << select2_bits) - 1))
	, m_page_shift(page_shift)
	, m_sampler(sampler_size)
	, m_enable_pref(0)
	, m_select1(0)
	, m_select2(0)
	, m_fpq_p0(fpq_size)
	, m_fpq_p1(fpq_size)
	, m_fpq_p2(fpq_size)
	, m_sampler_fifo_ptr(0)
	, m_fpq_p0_fifo_ptr(0)
	, m_fpq_p1_fifo_ptr(0)
	, m_fpq_p2_fifo_ptr(0)
	, m_h2p_last1_vpn(0)
	, m_h2p_last2_vpn(0)
	, m_h2p_have1(false)
	, m_h2p_have2(false)
	, m_masp_sets(masp_entries / masp_assoc)
	, m_masp_assoc(masp_assoc)
	, m_masp_rr_ptr(masp_entries / masp_assoc, 0)
	, m_masp_lookahead(masp_lookahead == 0 ? 1 : masp_lookahead)
	, m_masp_degree(masp_degree == 0 ? 1 : masp_degree)
	, m_current_instruction(false)
{
	(void)pq_size; // pq_size is no longer used; the TLB subsystem owns the PQ
	// Initialize Sampler entries
	for (auto &e : m_sampler) { e.valid = false; e.vpn = 0; e.free_distance = 0; }
	// Initialize FPQ entries
	for (auto &e : m_fpq_p0) { e.valid = false; e.vpn = 0; }
	for (auto &e : m_fpq_p1) { e.valid = false; e.vpn = 0; }
	for (auto &e : m_fpq_p2) { e.valid = false; e.vpn = 0; }
	// Initialize FDT
	std::memset(m_fdt, 0, sizeof(m_fdt));
	// Initialize MASP table
	m_masp_table.resize(masp_entries);
	for (auto &e : m_masp_table) { e.valid = false; e.pc_tag = 0; e.last_miss_vpn = 0; e.stride = 0; e.stride_valid = false; }

	// Zero stats
	std::memset(&m_stats, 0, sizeof(m_stats));

	registerAllStats(_core->getId());
}

AgileTLBPrefetcher::~AgileTLBPrefetcher() {}

// ═══════════════════════════════════════════════════════════════════
//  FDT helpers
// ═══════════════════════════════════════════════════════════════════

// FDT index mapping: the 14 free distances {-7..-1, +1..+7} are stored
// in a flat array.  Negative distances occupy indices 0..6, positive
// distances occupy indices 7..13.  Distance 0 is never used.
//
//   dist  -7  -6  -5  -4  -3  -2  -1  +1  +2  +3  +4  +5  +6  +7
//   idx    0   1   2   3   4   5   6   7   8   9  10  11  12  13

uint32_t AgileTLBPrefetcher::fdtIndexFromDistance(int8_t dist)
{
	if (dist < 0) return static_cast<uint32_t>(dist + 7);        // -7...-1 -> 0..6
	return static_cast<uint32_t>(dist - 1 + 7);                  // +1..+7  -> 7..13
}

// Increment the FDT counter for a given free distance, then check whether
// any counter has saturated.  If so, right-shift *all* 14 counters by 1
// (global decay), which adapts to phase changes by halving old evidence.
void AgileTLBPrefetcher::incrementFDTForDistance(int8_t dist)
{
	uint32_t idx = fdtIndexFromDistance(dist);
	satInc(m_fdt[idx], m_fdt_max);
	decayFDTIfNeeded();
}

void AgileTLBPrefetcher::decayFDTIfNeeded()
{
	for (uint32_t i = 0; i < 14; i++)
	{
		if (m_fdt[i] >= m_fdt_max)
		{
			for (uint32_t j = 0; j < 14; j++)
				m_fdt[j] >>= 1;
			m_stats.fdt_decay_events++;
			return;
		}
	}
}

// ═══════════════════════════════════════════════════════════════════
//  Free-meta & pending-VPN bookkeeping
// ═══════════════════════════════════════════════════════════════════
//
// ATP does NOT maintain its own Prefetch Queue.  The TLB subsystem
// owns the real PQ (entry_priority_queue inside TLB).  ATP pushes
// prefetched translations into the returned result vector, and the
// TLB inserts them into its PQ.
//
// However, ATP needs two pieces of side-state:
//
//   m_free_meta   : maps VPN → free_distance for SBFP free prefetches
//                   so that when a demand later consumes the PQ entry
//                   (pq_hit=true), we can credit the correct FDT counter.
//
//   m_pending_vpns: tracks VPNs currently awaiting materialization in
//                   the TLB's PQ, to avoid issuing duplicate walks.

void AgileTLBPrefetcher::recordFreeMeta(uint64_t vpn, int8_t free_distance)
{
	m_free_meta[vpn] = free_distance;
}

bool AgileTLBPrefetcher::lookupAndConsumeFreeMeta(uint64_t vpn, int8_t &free_distance_out)
{
	auto it = m_free_meta.find(vpn);
	if (it == m_free_meta.end()) return false;
	free_distance_out = it->second;
	m_free_meta.erase(it);
	return true;
}

bool AgileTLBPrefetcher::isPending(uint64_t vpn) const
{
	return m_pending_vpns.count(vpn) > 0;
}

void AgileTLBPrefetcher::markPending(uint64_t vpn)
{
	m_pending_vpns.insert(vpn);
}

void AgileTLBPrefetcher::consumePending(uint64_t vpn)
{
	m_pending_vpns.erase(vpn);
}

// ═══════════════════════════════════════════════════════════════════
//  Sampler operations
// ═══════════════════════════════════════════════════════════════════
//
// The Sampler is a 64-entry FIFO that stores (VPN, free_distance) pairs
// for free prefetch candidates that did NOT pass the FDT threshold and
// hence were not admitted into the real PQ.  Its purpose is FDT training:
// if a sampled VPN is later demanded (looked up on a PQ miss), we know
// that free distance was useful, so we increment its FDT counter.
// The Sampler is searched only off the critical path (on PQ misses).

bool AgileTLBPrefetcher::samplerLookup(uint64_t vpn, SamplerEntry &out, uint32_t &idx)
{
	for (uint32_t i = 0; i < m_sampler_size; i++)
	{
		if (m_sampler[i].valid && m_sampler[i].vpn == vpn)
		{
			out = m_sampler[i];
			idx = i;
			return true;
		}
	}
	return false;
}

void AgileTLBPrefetcher::samplerInstall(uint64_t vpn, int8_t free_distance)
{
	SamplerEntry &e = m_sampler[m_sampler_fifo_ptr];
	e.valid         = true;
	e.vpn           = vpn;
	e.free_distance = free_distance;
	m_sampler_fifo_ptr = (m_sampler_fifo_ptr + 1) % m_sampler_size;
}

// ═══════════════════════════════════════════════════════════════════
//  FPQ (Fake Prefetch Queue) operations
// ═══════════════════════════════════════════════════════════════════
//
// Each of the 3 child prefetchers (H2P, MASP, STP) has its own FPQ.
// On every demand miss, ALL three children compute their hypothetical
// predictions and insert the resulting VPNs (base predictions + SBFP
// free neighbors) into their respective FPQs.  Only VPNs are stored
// (no translations), since FPQs are used purely for accuracy tracking.
//
// Before making the ATP selection decision for the current miss, we
// probe all 3 FPQs: a hit in child i's FPQ means "child i would have
// caught this demand miss had it been selected last time".  The
// hit/miss pattern drives the Figure 7 truth-table counter updates.

bool AgileTLBPrefetcher::fpqLookup(const std::vector<FPQEntry> &fpq, uint64_t vpn) const
{
	for (uint32_t i = 0; i < static_cast<uint32_t>(fpq.size()); i++)
		if (fpq[i].valid && fpq[i].vpn == vpn) return true;
	return false;
}

void AgileTLBPrefetcher::fpqInstall(std::vector<FPQEntry> &fpq, uint32_t &fifo_ptr, uint64_t vpn)
{
	FPQEntry &e = fpq[fifo_ptr];
	e.valid = true;
	e.vpn   = vpn;
	fifo_ptr = (fifo_ptr + 1) % static_cast<uint32_t>(fpq.size());
}

// ═══════════════════════════════════════════════════════════════════
//  TLB containment check
// ═══════════════════════════════════════════════════════════════════
//
// Checks whether a VPN already has a valid entry in any level of the
// TLB hierarchy.  Used to filter out prefetches that would be useless
// because the translation is already cached.

bool AgileTLBPrefetcher::inAnyTLB(uint64_t vpn) const
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
//  Direct page table lookup (no modeled walk)
// ═══════════════════════════════════════════════════════════════════
//
// Performs an instantaneous PT query (no timing, no stats) to check
// whether a virtual page has a valid mapping.  Used for:
//   - Filtering child predictions before inserting into FPQs
//     (avoids polluting FPQs with unmapped pages)
//   - SBFP neighbor validation in fake (FPQ) mode

bool AgileTLBPrefetcher::directPageTableLookupVPN(PageTable *pt, uint64_t vpn,
												  uint64_t &ppn, uint32_t &page_size) const
{
	if (!pt) return false;
	IntPtr addr = static_cast<IntPtr>(vpn) << m_page_shift;
	PTWResult r = pt->initializeWalk(addr, /*count*/ false, /*is_prefetch*/ true, /*restart_walk*/ true);
	if (r.fault_happened || r.ppn == 0)
		return false;
	ppn       = static_cast<uint64_t>(r.ppn);
	page_size = static_cast<uint32_t>(r.page_size);
	return true;
}

// ═══════════════════════════════════════════════════════════════════
//  H2P predictor  (History-2-based Predictor)
// ═══════════════════════════════════════════════════════════════════
//
// Maintains a sliding window of the last 3 demand-miss VPNs: A, B, E
// (oldest to newest).  On a new miss to page E, H2P predicts:
//
//   prediction 1 = E + d(E, B)     i.e. repeat the most recent stride
//   prediction 2 = E + d(B, A)     i.e. repeat the previous stride
//
// This captures simple sequential and repeating-stride patterns that
// are common in array traversals.

std::vector<uint64_t> AgileTLBPrefetcher::predictH2P(uint64_t vpn) const
{
	std::vector<uint64_t> preds;
	if (!m_h2p_have2) return preds; // need at least 2 prior misses

	// E = vpn (current miss), B = m_h2p_last1_vpn, A = m_h2p_last2_vpn
	int64_t d_EB = static_cast<int64_t>(vpn) - static_cast<int64_t>(m_h2p_last1_vpn);
	int64_t d_BA = static_cast<int64_t>(m_h2p_last1_vpn) - static_cast<int64_t>(m_h2p_last2_vpn);

	int64_t p1 = static_cast<int64_t>(vpn) + d_EB;
	int64_t p2 = static_cast<int64_t>(vpn) + d_BA;

	if (p1 > 0) preds.push_back(static_cast<uint64_t>(p1));
	if (p2 > 0 && p2 != p1) preds.push_back(static_cast<uint64_t>(p2));
	return preds;
}

void AgileTLBPrefetcher::updateH2PHistory(uint64_t vpn)
{
	m_h2p_last2_vpn = m_h2p_last1_vpn;
	m_h2p_have2     = m_h2p_have1;
	m_h2p_last1_vpn = vpn;
	m_h2p_have1     = true;
}

// ═══════════════════════════════════════════════════════════════════
//  MASP predictor / table  (Modified Arbitrary Stride Predictor)
// ═══════════════════════════════════════════════════════════════════
//
// A PC-indexed, 64-entry, 4-way set-associative stride table.  Each
// entry stores {PC_tag, last_miss_vpn, stride}.  On a table hit for
// the current miss page A (with stored previous miss E and stride S):
//
//   prediction 1 = A + S           (repeat the learned stride)
//   prediction 2 = A + d(A, E)     (use the instantaneous delta)
//
// Then the entry is updated: stride ← A − E, last_miss_vpn ← A.
// On a table miss, a new entry is allocated (FIFO per-set replacement)
// with no predictions until the second miss from the same PC.

uint32_t AgileTLBPrefetcher::maspSetIndex(uint64_t eip) const
{
	return static_cast<uint32_t>(eip % m_masp_sets);
}

std::vector<uint64_t> AgileTLBPrefetcher::predictMASP(uint64_t vpn, uint64_t eip) const
{
	std::vector<uint64_t> preds;
	uint32_t set = maspSetIndex(eip);
	uint32_t base = set * m_masp_assoc;

	for (uint32_t w = 0; w < m_masp_assoc; w++)
	{
		const MASPEntry &e = m_masp_table[base + w];
		if (e.valid && e.pc_tag == eip)
		{
			// predict A + (lookahead + i) * S, for i in [0, degree)
			// (matches the baseline ASP lookahead/degree stride fan-out)
			if (e.stride_valid && e.stride != 0)
			{
				for (uint32_t i = 0; i < m_masp_degree; i++)
				{
					int64_t off = static_cast<int64_t>(m_masp_lookahead + i) * e.stride;
					int64_t p = static_cast<int64_t>(vpn) + off;
					if (p > 0) preds.push_back(static_cast<uint64_t>(p));
				}
			}
			// predict A + d(A, E)
			int64_t d = static_cast<int64_t>(vpn) - static_cast<int64_t>(e.last_miss_vpn);
			if (d != 0)
			{
				int64_t p2 = static_cast<int64_t>(vpn) + d;
				if (p2 > 0)
				{
					// Avoid duplicates
					bool dup = false;
					for (auto v : preds) if (v == static_cast<uint64_t>(p2)) { dup = true; break; }
					if (!dup) preds.push_back(static_cast<uint64_t>(p2));
				}
			}
			return preds;
		}
	}
	// Table miss – no predictions
	return preds;
}

void AgileTLBPrefetcher::updateMASP(uint64_t vpn, uint64_t eip)
{
	uint32_t set = maspSetIndex(eip);
	uint32_t base = set * m_masp_assoc;

	// Search for PC hit
	for (uint32_t w = 0; w < m_masp_assoc; w++)
	{
		MASPEntry &e = m_masp_table[base + w];
		if (e.valid && e.pc_tag == eip)
		{
			// Update stride and last miss
			e.stride       = static_cast<int64_t>(vpn) - static_cast<int64_t>(e.last_miss_vpn);
			e.stride_valid = true;
			e.last_miss_vpn = vpn;
			return;
		}
	}

	// Table miss – find invalid or replace first entry (FIFO-like per set)
	for (uint32_t w = 0; w < m_masp_assoc; w++)
	{
		MASPEntry &e = m_masp_table[base + w];
		if (!e.valid)
		{
			e.valid         = true;
			e.pc_tag        = eip;
			e.last_miss_vpn = vpn;
			e.stride        = 0;
			e.stride_valid  = false;
			return;
		}
	}

	// All ways occupied – round-robin replacement
	uint32_t way = m_masp_rr_ptr[set];
	m_masp_rr_ptr[set] = (way + 1) % m_masp_assoc;
	MASPEntry &victim = m_masp_table[base + way];
	victim.valid         = true;
	victim.pc_tag        = eip;
	victim.last_miss_vpn = vpn;
	victim.stride        = 0;
	victim.stride_valid  = false;
}

// ═══════════════════════════════════════════════════════════════════
//  STP predictor  (Simple Target Predictor)
// ═══════════════════════════════════════════════════════════════════
//
// The simplest child: on a miss to page A, STP always predicts the
// 4 immediate spatial neighbors {A−2, A−1, A+1, A+2}.  This works
// well for sequential or near-sequential access patterns where pages
// are touched in roughly linear order.

std::vector<uint64_t> AgileTLBPrefetcher::predictSTP(uint64_t vpn) const
{
	std::vector<uint64_t> preds;
	int64_t sv = static_cast<int64_t>(vpn);
	if (sv - 2 > 0) preds.push_back(static_cast<uint64_t>(sv - 2));
	if (sv - 1 > 0) preds.push_back(static_cast<uint64_t>(sv - 1));
	preds.push_back(static_cast<uint64_t>(sv + 1));
	preds.push_back(static_cast<uint64_t>(sv + 2));
	return preds;
}

// ═══════════════════════════════════════════════════════════════════
//  SBFP: process free PTEs from a completed page walk
// ═══════════════════════════════════════════════════════════════════
//
// When a page table walk fetches a leaf PTE from memory, the entire
// 64B cache line is brought in.  For 4KB pages (8-byte PTEs), that
// cache line contains 8 PTEs — the demanded one plus up to 7 neighbors.
// These neighbors are available "for free" (no extra memory access).
//
// This function examines each valid neighbor and decides:
//   - FDT[distance] > threshold  →  insert into real PQ (or FPQ in
//     fake mode) — this neighbor distance has been useful enough.
//   - Otherwise                  →  insert into Sampler (real mode
//     only) — give it a chance to prove useful for FDT training.
//
// Parameters:
//   insert_into_real_pq   true for demand/real-prefetch walks;
//                         false for fake walks (FPQ population).
//   fake_targets_out      if non-null, collects admitted VPNs for FPQ.
//   walk_completion_time  timestamp when the parent walk's cache-line
//                         fetch completes; free neighbors inherit this.
//   result_out            if non-null, pushes query_entries into the
//                         TLB's materialization queue.

void AgileTLBPrefetcher::processFreePTEsFromCompletedWalk(
	uint64_t demand_vpn, uint64_t /*demand_ppn*/, uint32_t demand_page_size,
	PageTable *pt, bool insert_into_real_pq,
	std::vector<uint64_t> *fake_targets_out,
	SubsecondTime walk_completion_time,
	std::vector<query_entry> *result_out)
{
	// SBFP free-neighbor math only applies to 4KB leaf PTEs.
	// For 2MB / 1GB pages the PTE is at a different radix level
	// with a different cache-line alignment, so skip.
	if (demand_page_size != m_page_shift) return;  // m_page_shift == 12 for 4KB

	// For 4KB pages: 64B cache line holds 8 PTEs (8B each)
	uint32_t pte_slot = static_cast<uint32_t>(demand_vpn & 0x7);

	for (int8_t dist = -7; dist <= 7; dist++)
	{
		if (dist == 0) continue;

		int32_t neighbor_slot = static_cast<int32_t>(pte_slot) + static_cast<int32_t>(dist);
		if (neighbor_slot < 0 || neighbor_slot >= 8) continue;

		uint64_t neighbor_vpn = static_cast<uint64_t>(static_cast<int64_t>(demand_vpn) + dist);

		uint32_t fdt_idx = fdtIndexFromDistance(dist);

		if (insert_into_real_pq)
			m_stats.free_candidates_seen++;

		m_stats.dist_seen[fdt_idx]++;

		// Direct lookup – no modeled walk
		uint64_t neighbor_ppn = 0;
		uint32_t neighbor_page_size = 0;
		if (!directPageTableLookupVPN(pt, neighbor_vpn, neighbor_ppn, neighbor_page_size))
			continue; // Translation doesn't exist or faults

		if (m_fdt[fdt_idx] > m_fdt_threshold) // strict >
		{
			if (insert_into_real_pq)
			{
				// Skip if already in TLB or pending in the TLB's PQ
				if (!inAnyTLB(neighbor_vpn) && !isPending(neighbor_vpn))
				{
					// Record free-meta so we can credit FDT on future PQ hit
					recordFreeMeta(neighbor_vpn, dist);
					markPending(neighbor_vpn);
					m_stats.free_inserted_pq++;
					m_stats.dist_pq[fdt_idx]++;

					// Push into the TLB's priority queue so the
					// translation is visible to future TLB lookups.
					if (result_out)
					{
						query_entry fq{};
						fq.address   = static_cast<IntPtr>(neighbor_vpn) << m_page_shift;
						fq.ppn       = static_cast<IntPtr>(neighbor_ppn);
						fq.page_size = static_cast<int>(neighbor_page_size);
						fq.timestamp = walk_completion_time;
						result_out->push_back(fq);
					}
				}
			}
			if (fake_targets_out)
			{
				fake_targets_out->push_back(neighbor_vpn);
			}
		}
		else
		{
			if (insert_into_real_pq)
			{
				samplerInstall(neighbor_vpn, dist);
				m_stats.free_inserted_sampler++;
				m_stats.dist_sampler[fdt_idx]++;
			}
		}
	}
}

std::vector<uint64_t> AgileTLBPrefetcher::getSBFPAdmittedFreeVPNs(uint64_t vpn, PageTable *pt)
{
	std::vector<uint64_t> result;
	uint32_t pte_slot = static_cast<uint32_t>(vpn & 0x7);

	for (int8_t dist = -7; dist <= 7; dist++)
	{
		if (dist == 0) continue;
		int32_t neighbor_slot = static_cast<int32_t>(pte_slot) + static_cast<int32_t>(dist);
		if (neighbor_slot < 0 || neighbor_slot >= 8) continue;

		uint64_t neighbor_vpn = static_cast<uint64_t>(static_cast<int64_t>(vpn) + dist);
		uint32_t fdt_idx = fdtIndexFromDistance(dist);

		if (m_fdt[fdt_idx] > m_fdt_threshold)
		{
			uint64_t ppn; uint32_t ps;
			if (directPageTableLookupVPN(pt, neighbor_vpn, ppn, ps))
				result.push_back(neighbor_vpn);
		}
	}
	return result;
}

// ═══════════════════════════════════════════════════════════════════
//  ATP counter truth table (Figure 7)
// ═══════════════════════════════════════════════════════════════════

// The truth table encodes the intuition:
//   - If at least one child predicted the miss → prefetching is useful → C0++
//   - If no child predicted it → prefetching is harmful → C0--
//   - C1 biases toward P0 (H2P) when it hits and others don't, away when
//     P1 or P2 hit but P0 doesn't.
//   - C2 picks between P1 (MASP) and P2 (STP) in the "not P0" subtree.
void AgileTLBPrefetcher::updateATPCounters(bool h0, bool h1, bool h2)
{
	// P0=H2P, P1=MASP, P2=STP
	// H = hit in FPQ, M = miss in FPQ
	if (!h0 && !h1 && !h2) {
		// M M M -> C0--  (no child would have helped → less confidence in prefetching)
		satDec(m_enable_pref); m_stats.enable_pref_dec++;
	} else if (!h0 && !h1 && h2) {
		// M M H -> C0++, C1--, C2++
		satInc(m_enable_pref, m_enable_pref_max); m_stats.enable_pref_inc++;
		satDec(m_select1);                        m_stats.select1_dec++;
		satInc(m_select2, m_select2_max);         m_stats.select2_inc++;
	} else if (!h0 && h1 && !h2) {
		// M H M -> C0++, C1--, C2--
		satInc(m_enable_pref, m_enable_pref_max); m_stats.enable_pref_inc++;
		satDec(m_select1);                        m_stats.select1_dec++;
		satDec(m_select2);                        m_stats.select2_dec++;
	} else if (!h0 && h1 && h2) {
		// M H H -> C0++, C1--
		satInc(m_enable_pref, m_enable_pref_max); m_stats.enable_pref_inc++;
		satDec(m_select1);                        m_stats.select1_dec++;
	} else if (h0 && !h1 && !h2) {
		// H M M -> C0++, C1++
		satInc(m_enable_pref, m_enable_pref_max); m_stats.enable_pref_inc++;
		satInc(m_select1, m_select1_max);         m_stats.select1_inc++;
	} else if (h0 && !h1 && h2) {
		// H M H -> C0++, C2++
		satInc(m_enable_pref, m_enable_pref_max); m_stats.enable_pref_inc++;
		satInc(m_select2, m_select2_max);         m_stats.select2_inc++;
	} else if (h0 && h1 && !h2) {
		// H H M -> C0++, C2--
		satInc(m_enable_pref, m_enable_pref_max); m_stats.enable_pref_inc++;
		satDec(m_select2);                        m_stats.select2_dec++;
	} else /* h0 && h1 && h2 */ {
		// H H H -> C0++
		satInc(m_enable_pref, m_enable_pref_max); m_stats.enable_pref_inc++;
	}
}

// ═══════════════════════════════════════════════════════════════════
//  ATP child selection
// ═══════════════════════════════════════════════════════════════════
//
// Decision tree using the MSB of each counter:
//
//   enable_pref MSB == 0 ?  →  DISABLE (suppress all prefetching)
//        │ 1
//   select_1 MSB == 1 ?     →  P0 (H2P)
//        │ 0
//   select_2 MSB == 1 ?     →  P2 (STP)
//        │ 0
//                           →  P1 (MASP)

AgileTLBPrefetcher::ATPChild AgileTLBPrefetcher::chooseATPChild() const
{
	if (!msb(m_enable_pref, m_enable_pref_bits))
		return DISABLE;
	if (msb(m_select1, m_select1_bits))
		return P0_H2P;
	if (msb(m_select2, m_select2_bits))
		return P2_STP;
	return P1_MASP;
}

// ═══════════════════════════════════════════════════════════════════
//  performPrefetch – main entry point
// ═══════════════════════════════════════════════════════════════════
//
// Called by the TLB on every access.  We only act on TLB misses.
// The algorithm follows these steps (see header for the full picture):
//
//  1. Probe ATP's internal PQ for a previously prefetched translation.
//  2. On PQ miss, probe the Sampler for FDT training feedback.
//  3. On PQ miss, issue a demand page walk; run SBFP on completion.
//  4. Probe all 3 FPQs to see which children would have predicted this.
//  5. Update the 3 ATP counters via the Figure 7 truth table.
//  6. Choose which child (if any) to use for real prefetching.
//  7. Issue real prefetch walks for the chosen child's predictions;
//     run SBFP after each successful walk.
//  8. Populate all 3 FPQs using each child's hypothetical predictions
//     (including SBFP free neighbors that would be admitted).
//  9. Update H2P miss history and MASP stride table.
//
// Returns query_entries for the TLB's priority queue (both PQ hits
// and newly-issued prefetch translations).

std::vector<query_entry> AgileTLBPrefetcher::performPrefetch(
	IntPtr address, IntPtr eip, Core::lock_signal_t lock,
	bool modeled, bool count, PageTable *pt,
	bool instruction, bool tlb_hit, bool pq_hit, int page_size)
{
	std::vector<query_entry> result;

	m_current_instruction = instruction;

	uint64_t vpn = static_cast<uint64_t>(address) >> m_page_shift;

	// ── 1. Handle PQ hit (the TLB already served the translation) ──
	// On a PQ hit the TLB subsystem found a previously prefetched entry.
	// We check our free-meta side-table to see if this was an SBFP free
	// prefetch, and if so, credit the FDT counter for that distance.
	if (pq_hit)
	{
		m_stats.pq_hits++;
		consumePending(vpn);

		int8_t free_dist;
		if (lookupAndConsumeFreeMeta(vpn, free_dist))
		{
			incrementFDTForDistance(free_dist);
			m_stats.fdt_updates_from_pq++;
			uint32_t didx = fdtIndexFromDistance(free_dist);
			m_stats.dist_hits[didx]++;
		}
	}

	// On any TLB hit (regular or PQ-sourced), skip the demand walk
	// and all prediction / FPQ logic.
	if (tlb_hit) return result;

	m_stats.queries++;

	// ── 2. Sampler lookup on TLB miss ───────────────────────────
	SamplerEntry samp_out;
	uint32_t samp_idx = 0;
	if (samplerLookup(vpn, samp_out, samp_idx))
	{
		m_stats.sampler_hits++;
		incrementFDTForDistance(samp_out.free_distance);
		m_stats.fdt_updates_from_sampler++;
		uint32_t didx = fdtIndexFromDistance(samp_out.free_distance);
		m_stats.dist_hits[didx]++;
		m_sampler[samp_idx].valid = false;
	}
	else
	{
		m_stats.sampler_misses++;
	}

	// ── 3. Demand page walk ─────────────────────────────────────
	m_stats.demand_walks++;
	query_entry demand_q = PTWTransparent(address, eip, lock, modeled, count, pt);
	if (demand_q.ppn != 0)
	{
		m_stats.demand_walks_successful++;
		// Run SBFP on completed demand walk
		processFreePTEsFromCompletedWalk(
			vpn, static_cast<uint64_t>(demand_q.ppn),
			static_cast<uint32_t>(demand_q.page_size),
			pt, /*insert_into_real_pq*/ true, /*fake_targets_out*/ nullptr,
			demand_q.timestamp, &result);
	}
	else
	{
		m_stats.demand_walks_failed++;
	}

	// ── 4. Probe all 3 FPQs ─────────────────────────────────────
	bool h0 = fpqLookup(m_fpq_p0, vpn);
	bool h1 = fpqLookup(m_fpq_p1, vpn);
	bool h2 = fpqLookup(m_fpq_p2, vpn);

	if (h0) m_stats.fpq_p0_hits++;
	if (h1) m_stats.fpq_p1_hits++;
	if (h2) m_stats.fpq_p2_hits++;

	// ── 5. Update C0/C1/C2 using Figure 7 truth table ──────────
	updateATPCounters(h0, h1, h2);

	// ── 6. Choose one real child ────────────────────────────────
	ATPChild choice = chooseATPChild();
	switch (choice) {
		case DISABLE: m_stats.atp_choice_disable++; break;
		case P0_H2P:  m_stats.atp_choice_h2p++;     break;
		case P1_MASP: m_stats.atp_choice_masp++;     break;
		case P2_STP:  m_stats.atp_choice_stp++;      break;
	}

	// ── 7. Real prefetch issue from chosen child ────────────────
	if (choice != DISABLE)
	{
		std::vector<uint64_t> preds;
		switch (choice) {
			case P0_H2P:
				preds = predictH2P(vpn);
				m_stats.real_prefetch_candidates_h2p += preds.size();
				break;
			case P1_MASP:
				preds = predictMASP(vpn, static_cast<uint64_t>(eip));
				m_stats.real_prefetch_candidates_masp += preds.size();
				break;
			case P2_STP:
				preds = predictSTP(vpn);
				m_stats.real_prefetch_candidates_stp += preds.size();
				break;
			default: break;
		}

		for (uint64_t target_vpn : preds)
		{
			if (target_vpn == 0) continue;
			if (inAnyTLB(target_vpn)) continue;
			if (isPending(target_vpn)) continue;

			m_stats.real_prefetch_walks_issued++;

			IntPtr target_addr = static_cast<IntPtr>(target_vpn) << m_page_shift;
			query_entry q = PTWTransparent(target_addr, eip, lock, modeled, count, pt);

			if (q.ppn != 0)
			{
				m_stats.real_prefetch_successful++;
				markPending(target_vpn);

				// SBFP on completed prefetch walk
				processFreePTEsFromCompletedWalk(
					target_vpn, static_cast<uint64_t>(q.ppn),
					static_cast<uint32_t>(q.page_size),
					pt, /*insert_into_real_pq*/ true, /*fake_targets_out*/ nullptr,
					q.timestamp, &result);

				result.push_back(q);
			}
			else
			{
				m_stats.real_prefetch_failed++;
			}
		}
	}

	// ── 8. Update all FPQs using fake predictions ───────────────
	// For each child, compute what it *would* have predicted, confirm
	// the translation exists (via direct PT lookup, no modeled walk),
	// and insert the VPN + its SBFP-admitted free neighbors into that
	// child's FPQ.  This is how ATP learns which child is best.
	//
	// H2P FPQ
	{
		std::vector<uint64_t> fake_h2p = predictH2P(vpn);
		for (uint64_t fvpn : fake_h2p)
		{
			uint64_t ppn; uint32_t ps;
			if (fvpn != 0 && directPageTableLookupVPN(pt, fvpn, ppn, ps))
			{
				m_stats.fpq_fake_base_preds_p0++;
				fpqInstall(m_fpq_p0, m_fpq_p0_fifo_ptr, fvpn);
				m_stats.fpq_p0_inserts++;
				// SBFP free neighbors for this fake walk
				std::vector<uint64_t> free_vpns = getSBFPAdmittedFreeVPNs(fvpn, pt);
				for (uint64_t fv : free_vpns)
				{
					m_stats.fpq_fake_free_preds_p0++;
					fpqInstall(m_fpq_p0, m_fpq_p0_fifo_ptr, fv);
					m_stats.fpq_p0_inserts++;
				}
			}
		}
	}
	// MASP FPQ
	{
		std::vector<uint64_t> fake_masp = predictMASP(vpn, static_cast<uint64_t>(eip));
		for (uint64_t fvpn : fake_masp)
		{
			uint64_t ppn; uint32_t ps;
			if (fvpn != 0 && directPageTableLookupVPN(pt, fvpn, ppn, ps))
			{
				m_stats.fpq_fake_base_preds_p1++;
				fpqInstall(m_fpq_p1, m_fpq_p1_fifo_ptr, fvpn);
				m_stats.fpq_p1_inserts++;
				std::vector<uint64_t> free_vpns = getSBFPAdmittedFreeVPNs(fvpn, pt);
				for (uint64_t fv : free_vpns)
				{
					m_stats.fpq_fake_free_preds_p1++;
					fpqInstall(m_fpq_p1, m_fpq_p1_fifo_ptr, fv);
					m_stats.fpq_p1_inserts++;
				}
			}
		}
	}
	// STP FPQ
	{
		std::vector<uint64_t> fake_stp = predictSTP(vpn);
		for (uint64_t fvpn : fake_stp)
		{
			uint64_t ppn; uint32_t ps;
			if (fvpn != 0 && directPageTableLookupVPN(pt, fvpn, ppn, ps))
			{
				m_stats.fpq_fake_base_preds_p2++;
				fpqInstall(m_fpq_p2, m_fpq_p2_fifo_ptr, fvpn);
				m_stats.fpq_p2_inserts++;
				std::vector<uint64_t> free_vpns = getSBFPAdmittedFreeVPNs(fvpn, pt);
				for (uint64_t fv : free_vpns)
				{
					m_stats.fpq_fake_free_preds_p2++;
					fpqInstall(m_fpq_p2, m_fpq_p2_fifo_ptr, fv);
					m_stats.fpq_p2_inserts++;
				}
			}
		}
	}

	// ── 9. Update histories / tables ────────────────────────────
	updateH2PHistory(vpn);
	updateMASP(vpn, static_cast<uint64_t>(eip));

	return result;
}

} // namespace ParametricDramDirectoryMSI
