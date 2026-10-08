#pragma once
// ============================================================================
// Utopia MMU -- translation over RestSegs and the FlexSeg
// Based on: "Utopia: Fast and Efficient Address Translation via Hybrid
//            Restrictive & Flexible Virtual-to-Physical Address Mappings",
//            Kanellopoulos et al., MICRO 2023
// ============================================================================
//
// The Utopia allocator (utopia.h) places a page either in a RestSeg -- a
// set-associative segment where the physical address follows from the set
// the VPN selects and the way the page was given -- or in the FlexSeg, which
// the page table maps.  This MMU translates both:
//
//   * TLB hierarchy lookup, as the baseline MMU's.
//   * On a miss, in parallel: the UTLB (a compact TLB of RestSeg translations,
//     utopia_tlb.h) and the RestSeg walk (RSW).  The RSW reads, per RestSeg,
//     either the core's fingerprint array and tag array (FPA + TAR, through
//     the fpacache/tarcache and then the cache hierarchy), or the
//     application's radix way table (one line per level, through the PWC and
//     the cache hierarchy; radix_lookup_enabled).  With two RestSegs the walk
//     is parallel until one of them dominates the hits (adaptive_rsw).
//   * On an RSW miss, the page table walk; a fault goes to the Utopia fault
//     handler, after which the page is either in a RestSeg (walked) or in the
//     FlexSeg (walked again, now mapped).
//   * A FlexSeg page the walker serviced may be migrated into the 4KB RestSeg
//     by the allocator's cost-based Top-K policy (or the legacy threshold
//     one): TLBs and metadata caches are shot down for it and for the page
//     its arrival evicted, and a migration stall is charged to the next
//     accesses of the page.
//   * CATS (confidence-aware throttled speculation): with FPA fingerprints,
//     the data lines of the candidate ways can be prefetched into the L2
//     before the tag array answers, gated by a per-set confidence and a way
//     predictor (speculation/enabled).
//
// Counters register under the MMU's name; the per-RestSeg ones carry the
// RestSeg index as a suffix (fpa_accesses_restseg0, ...).
// ============================================================================

#include "../memory_manager.h"
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "shmem_perf_model.h"
#include "pagetable.h"
#include "tlb_subsystem.h"
#include "mmu_base.h"
#include "utopia_cache_template.h"
#include "utopia_tlb.h"
#include "ptmshrs.h"
#include "base_filter.h"
#include "sim_log.h"
#include "debug_config.h"
#include <unordered_map>
#include <vector>

class PhysicalMemoryAllocator;
template <typename Policy> class UtopiaAllocator;
namespace Sniper { namespace Utopia { struct MetricsPolicy; } }

namespace ParametricDramDirectoryMSI
{
	class TLBHierarchy;

	// A functional RestSeg lookup: no cache accesses, no latency.  On a hit,
	// the translation and the metadata lines a walk would read; on a miss
	// (and on a hit) the ways whose fingerprint matches, for CATS.
	struct RSWLookupResult {
		bool hit;
		int seg_id;
		int way;
		int page_size_bits;
		IntPtr ppn;                 // 4KB units
		IntPtr physical_address;
		IntPtr virtual_address;
		IntPtr fpa_address;
		IntPtr tar_address;
		int set_index;
		static const int MAX_CANDIDATES = 4;
		int num_candidates;
		int candidate_ways[MAX_CANDIDATES];
		IntPtr candidate_pas[MAX_CANDIDATES];

		RSWLookupResult() : hit(false), seg_id(-1), way(-1), page_size_bits(0), ppn(0), physical_address(0),
		                    virtual_address(0), fpa_address(0), tar_address(0), set_index(0), num_candidates(0) {
			for (int i = 0; i < MAX_CANDIDATES; i++) {
				candidate_ways[i] = -1;
				candidate_pas[i] = 0;
			}
		}
	};

	// What CATS decided for one translation
	struct SpecDecision {
		bool do_speculate;
		int chosen_way0, chosen_way1;
		IntPtr prefetch_addr0, prefetch_addr1;
		SpecDecision() : do_speculate(false), chosen_way0(-1), chosen_way1(-1), prefetch_addr0(0), prefetch_addr1(0) {}
	};

	class MemoryManagementUnitUtopia : public MemoryManagementUnitBase
	{
	protected:
		typedef UtopiaAllocator<Sniper::Utopia::MetricsPolicy> UtopiaAlloc;

		MemoryManagerBase *memory_manager;
		TLBHierarchy *tlb_subsystem;
		MSHR *pt_walkers;
		BaseFilter *ptw_filter;

		UtopiaCache *fpa_cache;
		UtopiaCache *tar_cache;
		UtopiaTLB *utlb;
		UtopiaAlloc *m_utopia;                  // the allocator, or NULL under another one (no RestSegs)
		int num_restsegs;

		std::unordered_map<IntPtr, std::pair<int, int>> ptw_stats;      // legacy migration: walks and DRAM accesses per 4KB VPN
		std::unordered_map<IntPtr, SubsecondTime> migration_queue;       // 4KB VPN -> when its migration completes

		// VA-PA consistency checks, as the baseline MMU's
		bool sanity_checks_enabled;
		std::unordered_map<IntPtr, IntPtr> va_to_pa_map;
		std::unordered_map<IntPtr, IntPtr> pa_to_va_map;
		UInt64 sanity_check_violations;

		// Configuration
		int ptw_migration_threshold;
		int dram_accesses_migration_threshold;
		bool legacy_migration_enabled;
		bool utlb_enabled;
		bool cost_topk_migration_enabled;   // cached from the allocator
		int cost_score_bits;
		int cost_ptw_base_inc;
		int cost_dram_inc_cap;
		bool radix_lookup_enabled;          // radix way table instead of FPA + TAR
		bool disable_fp_cache;              // route the FPA (TAR) reads past the dedicated cache,
		bool disable_tar_cache;             //   tagged PAGE_TABLE_DATA so the PWC path sees them
		bool adaptive_rsw_enabled;
		double rsw_sequential_threshold;
		UInt64 rsw_warmup_accesses;

		// Adaptive RSW state
		std::vector<UInt64> rsw_hits_per_restseg;
		UInt64 rsw_total_hits;
		int rsw_dominant_restseg;

		// CATS
		struct SpecCtrl {
			bool enabled;
			int wmax;                     // at most this many candidate ways
			int conf_bits;
			int conf_threshold;
			int conf_table_entries;
			bool way_predictor_enabled;
			int lastway_table_entries;
			bool enable_instruction_speculation;
			struct ConfEntry { UInt16 tag; UInt8 conf; ConfEntry() : tag(0xFFFF), conf(0) {} };
			struct WayEntry { UInt16 tag; UInt8 last_way; UInt8 usefulness; WayEntry() : tag(0xFFFF), last_way(0), usefulness(0) {} };
			std::vector<ConfEntry> conf_table;
			std::vector<WayEntry> way_table;
			SpecCtrl() : enabled(false), wmax(2), conf_bits(3), conf_threshold(3), conf_table_entries(16384),
			             way_predictor_enabled(true), lastway_table_entries(8192), enable_instruction_speculation(false) {}
		};
		SpecCtrl spec;

		SimLog *mmu_log;

		// The counters of a radix way-table walk: the lines through the PWC
		// and, on a miss, where the cache hierarchy served them (the 4KB
		// table's; step 24's 2MB table has its own)
		struct RadixWalkCounters {
			UInt64 pwc_hits, pwc_misses;
			UInt64 internal_hit_l2, internal_hit_nuca, internal_hit_dram, internal_accesses;
			UInt64 leaf_hit_l2, leaf_hit_nuca, leaf_hit_dram, leaf_accesses;
		};

		// Per-RestSeg counters
		struct RestSegStats {
			UInt64 rsw_hits;
			SubsecondTime fpa_latency, tar_latency, radix_latency;
			UInt64 fpa_accesses, tar_accesses, fpa_cache_hits, tar_cache_hits, fp_filtered;
			UInt64 fpa_hit_l2, fpa_hit_nuca, fpa_hit_dram, tar_hit_l2, tar_hit_nuca, tar_hit_dram;
			UInt64 radix_lookups, radix_hits, radix_misses;
		};
		std::vector<RestSegStats> rs_stats;

		struct TranslationStats
		{
			UInt64 num_translations;
			UInt64 num_translations_instruction;
			UInt64 num_translations_data;
			UInt64 page_faults;
			UInt64 page_faults_instruction;
			UInt64 page_faults_data;
			UInt64 page_table_walks;
			UInt64 l2_tlb_misses;
			UInt64 tlb_hits_instruction, tlb_hits_data;     // the names tools/sniper_lib.py maps the
			UInt64 tlb_misses_instruction, tlb_misses_data; //   legacy itlb/dtlb counters to
			UInt64 flextorest_migrations;
			UInt64 tlb_shootdowns;
			UInt64 requests_affected_by_migration;
			SubsecondTime migration_stall_cycles;
			UInt64 data_in_restseg;            // translated by an RSW after a fault
			UInt64 data_in_flexseg;            // translated by a walk after a fault
			UInt64 data_in_tlb;
			UInt64 data_in_utlb;
			UInt64 data_in_restseg_no_fault;   // RSW hit
			UInt64 data_in_flexseg_no_fault;   // walk, no fault
			UInt64 total_rsw_memory_requests;
			UInt64 rsw_sequential_accesses;
			UInt64 rsw_parallel_accesses;
			UInt64 meta_accesses_routed_to_ptw;
			UInt64 meta_accesses_routed_to_fp_tar;
			RadixWalkCounters radix;           // radix_pwc_hits, radix_leaf_accesses, ...
			// CATS
			UInt64 cats_spec_attempts, cats_spec_correct, cats_spec_wrong_way, cats_spec_miss;
			UInt64 cats_gated_ambiguity, cats_gated_confidence, cats_gated_instruction;
			UInt64 cats_prefetch_issued, cats_conf_updates, cats_way_predictions, cats_way_prediction_hits;
			UInt64 cats_considered, cats_cold_spec_issued;
			// Cost-based migration, this core's view
			UInt64 cost_topk_score_updates, cost_topk_migration_attempts, cost_topk_migrations_issued;
			SubsecondTime total_walk_latency_flexseg_no_fault;
			SubsecondTime total_rsw_latency_restseg_no_fault;
			SubsecondTime total_walk_latency_flexseg;
			SubsecondTime total_rsw_latency_restseg;
			SubsecondTime total_tlb_latency_on_tlb_hit;
			SubsecondTime total_utlb_latency;
			SubsecondTime total_walk_latency;
			SubsecondTime total_rsw_latency;
			SubsecondTime total_tlb_latency;
			SubsecondTime total_fault_latency;
			SubsecondTime total_translation_latency;
			SubsecondTime *tlb_latency_per_level;
		};
		TranslationStats translation_stats;

		// CATS helpers
		UInt64 computeSpecTableKey(int seg_id, int set_index);
		SpecCtrl::ConfEntry &lookupConfEntry(UInt64 key);
		int lookupWayPredictor(UInt64 key);
		SpecDecision decideSpeculation(const RSWLookupResult &lr, bool instruction, bool count);
		void updateCATSOutcome(const RSWLookupResult &lr, const SpecDecision &decision, bool hit, int correct_way, bool metadata_fast, bool count);
		void executeSpeculation(const SpecDecision &decision, IntPtr eip, SubsecondTime t_start, bool count);

		// Where a metadata access was served, into the per-RestSeg counters
		void countHitWhere(HitWhere::where_t where, UInt64 &l2, UInt64 &nuca, UInt64 &dram);

		// Register (and zero) a walk's counters under prefix_*
		void registerRadixWalkCounters(const String &prefix, RadixWalkCounters &c);
		// Charge a walk: the lines root first, in sequence
		SubsecondTime chargeRadixWalk(const std::vector<std::pair<IntPtr, bool>> &level_addresses, SubsecondTime start,
		                              IntPtr eip, Core::lock_signal_t lock, bool modeled, bool count, bool instruction,
		                              RadixWalkCounters &c);

		// Migration bookkeeping shared by the two policies
		void completeMigration(IntPtr address, IntPtr old_ppn, IntPtr new_ppn, int page_size, UInt64 vpn_4kb, bool count);

	public:
		MemoryManagementUnitUtopia(Core *core, MemoryManagerBase *memory_manager, ShmemPerfModel *shmem_perf_model, String name, MemoryManagementUnitBase *nested_mmu);
		~MemoryManagementUnitUtopia();

		void instantiatePageTableWalker();
		void instantiateTLBSubsystem();
		void instantiateRestSegWalker();
		void instantiateUTLB();
		void registerMMUStats();

		IntPtr performAddressTranslation(IntPtr eip, IntPtr address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count);

		// The RestSeg walk: (page size bits or -1, PPN, latency)
		virtual std::tuple<int, IntPtr, SubsecondTime> RestSegWalk(IntPtr address, bool instruction, IntPtr eip, Core::lock_signal_t lock, bool modeled, bool count);

		// The FlexSeg walk: a functional walk (no restart on a fault) charged
		// through filterPTWResult and calculatePTWCycles when it does not fault
		SubsecondTime walkFlexSeg(IntPtr address, bool count, bool modeled, IntPtr eip, Core::lock_signal_t lock,
		                          PageTable *page_table, bool instruction,
		                          int &out_page_size, IntPtr &out_ppn, bool &out_fault, int &out_requested_frames);

		RSWLookupResult functionalRSWLookup(IntPtr address);

		PTWResult filterPTWResult(IntPtr virtual_address, PTWResult ptw_result, PageTable *page_table, bool count);
		BaseFilter *getPTWFilter() override { return ptw_filter; }

		// After a migration: drop the page from every TLB, the UTLB and the
		// FPA/TAR caches
		void performTLBShootdown(IntPtr address, int page_size);
		void updateSanityCheckMaps(IntPtr address, IntPtr old_ppn, IntPtr new_ppn, int page_size);
		void invalidateSanityCheckMaps(IntPtr address, int page_size);

		void discoverVMAs();
	};
}
