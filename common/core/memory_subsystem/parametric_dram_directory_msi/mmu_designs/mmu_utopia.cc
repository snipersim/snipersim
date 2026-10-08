// ============================================================================
// Utopia MMU -- translation over RestSegs and the FlexSeg (see mmu_utopia.h)
// ============================================================================

#include "mmu_utopia.h"
#include "mmu_base.h"
#include "memory_manager.h"
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "thread.h"
#include "simulator.h"
#include "config.hpp"
#include "stats.h"
#include "utopia_cache_template.h"
#include "memory_management/physical_memory_allocators/utopia.h"
#include "misc/exception_handler_base.h"
#include "mimicos.h"
#include "instruction.h"
#include "filter_factory.h"
#include "debug_config.h"
#include <iostream>
#include <algorithm>

using namespace std;

namespace ParametricDramDirectoryMSI
{

// ============================================================================
// Construction / Destruction
// ============================================================================

	MemoryManagementUnitUtopia::MemoryManagementUnitUtopia(Core *_core, MemoryManagerBase *_memory_manager, ShmemPerfModel *_shmem_perf_model, String _name, MemoryManagementUnitBase *_nested_mmu)
		: MemoryManagementUnitBase(_core, _memory_manager, _shmem_perf_model, _name, _nested_mmu), memory_manager(_memory_manager)
	{
		mmu_log = new SimLog("MMU_UTOPIA", core->getId(), DEBUG_UTOPIA);
		std::cout << "[MMU::Utopia] Initializing Utopia MMU for core " << core->getId() << std::endl;

		if (Sim()->getMimicOS()->isUserspaceMimicosEnabled())
		{
			std::cerr << "[MMU::Utopia] The Utopia allocator lives in the sniper-space MimicOS; the userspace kernel cannot place pages in RestSegs" << std::endl;
			abort();
		}
		// The allocator: a Utopia one gives the RestSegs to walk; any other
		// leaves this MMU translating through the page table alone
		m_utopia = dynamic_cast<UtopiaAlloc *>(Sim()->getMimicOS()->getMemoryAllocator());
		num_restsegs = m_utopia ? m_utopia->getNumRestSegs() : 0;
		if (!m_utopia && core->getId() == 0)
			std::cout << "[MMU::Utopia] The memory allocator is not the Utopia one: no RestSegs, every page is walked" << std::endl;

		instantiatePageTableWalker();
		instantiateTLBSubsystem();
		instantiateRestSegWalker();
		instantiateUTLB();
		registerMMUStats();
	}

	MemoryManagementUnitUtopia::~MemoryManagementUnitUtopia()
	{
		if (sanity_checks_enabled)
			std::cout << "[MMU Sanity] Core " << core->getId() << " checked " << va_to_pa_map.size() << " unique VA->PA mappings, "
			          << pa_to_va_map.size() << " unique PA->VA mappings, " << sanity_check_violations << " violations detected" << std::endl;
		delete mmu_log;
		delete utlb;
		delete fpa_cache;
		delete tar_cache;
		delete tlb_subsystem;
		delete pt_walkers;
		delete[] translation_stats.tlb_latency_per_level;
	}

// ============================================================================
// Initialisation
// ============================================================================

	void MemoryManagementUnitUtopia::instantiatePageTableWalker()
	{
		legacy_migration_enabled = Sim()->getCfg()->getIntDefault("perf_model/utopia/legacy_migration_enabled", 0) != 0;
		ptw_migration_threshold = Sim()->getCfg()->getIntDefault("perf_model/utopia/ptw_migration_threshold", 2);
		dram_accesses_migration_threshold = Sim()->getCfg()->getIntDefault("perf_model/utopia/dram_accesses_migration_threshold", 2);

		String filter_type = Sim()->getCfg()->getString("perf_model/" + name + "/ptw_filter_type");
		ptw_filter = FilterPTWFactory::createFilterPTWBase(filter_type, name, core);
		pt_walkers = new MSHR(Sim()->getCfg()->getInt("perf_model/" + name + "/page_table_walkers"));
	}

	void MemoryManagementUnitUtopia::instantiateTLBSubsystem()
	{
		tlb_subsystem = new TLBHierarchy(name, core, memory_manager, shmem_perf_model);
	}

	void MemoryManagementUnitUtopia::instantiateRestSegWalker()
	{
		// The per-core FPA and TAR caches; sizes in KB, 64-byte lines
		int fpa_cache_size = Sim()->getCfg()->getInt("perf_model/utopia/fpacache/size");
		int fpa_cache_assoc = Sim()->getCfg()->getInt("perf_model/utopia/fpacache/assoc");
		ComponentLatency fpa_access(core->getDvfsDomain(), Sim()->getCfg()->getInt("perf_model/utopia/fpacache/access_penalty"));
		ComponentLatency fpa_miss(core->getDvfsDomain(), Sim()->getCfg()->getInt("perf_model/utopia/fpacache/miss_penalty"));
		fpa_cache = new UtopiaCache("fpacache", "perf_model/utopia/fpacache", core->getId(), 64, fpa_cache_size, fpa_cache_assoc, fpa_access, fpa_miss);

		int tag_cache_size = Sim()->getCfg()->getInt("perf_model/utopia/tagcache/size");
		int tag_cache_assoc = Sim()->getCfg()->getInt("perf_model/utopia/tagcache/assoc");
		ComponentLatency tar_access(core->getDvfsDomain(), Sim()->getCfg()->getInt("perf_model/utopia/tagcache/access_penalty"));
		ComponentLatency tar_miss(core->getDvfsDomain(), Sim()->getCfg()->getInt("perf_model/utopia/tagcache/miss_penalty"));
		tar_cache = new UtopiaCache("tarcache", "perf_model/utopia/tagcache", core->getId(), 64, tag_cache_size, tag_cache_assoc, tar_access, tar_miss);

		// Adaptive RSW: sequential once one RestSeg dominates the hits
		adaptive_rsw_enabled = Sim()->getCfg()->getBoolDefault("perf_model/utopia/adaptive_rsw_enabled", true);
		rsw_sequential_threshold = Sim()->getCfg()->hasKey("perf_model/utopia/rsw_sequential_threshold") ?
			Sim()->getCfg()->getFloat("perf_model/utopia/rsw_sequential_threshold") : 0.90;
		rsw_warmup_accesses = Sim()->getCfg()->getIntDefault("perf_model/utopia/rsw_warmup_accesses", 1000);
		rsw_hits_per_restseg.assign(num_restsegs > 0 ? num_restsegs : 1, 0);
		rsw_total_hits = 0;
		rsw_dominant_restseg = -1;

		// CATS
		spec.enabled = Sim()->getCfg()->getBoolDefault("perf_model/utopia/speculation/enabled", false);
		spec.wmax = Sim()->getCfg()->getIntDefault("perf_model/utopia/speculation/wmax", 2);
		spec.conf_bits = Sim()->getCfg()->getIntDefault("perf_model/utopia/speculation/conf_bits", 3);
		spec.conf_threshold = Sim()->getCfg()->getIntDefault("perf_model/utopia/speculation/conf_threshold", 3);
		spec.conf_table_entries = Sim()->getCfg()->getIntDefault("perf_model/utopia/speculation/conf_table_entries", 16384);
		spec.way_predictor_enabled = Sim()->getCfg()->getBoolDefault("perf_model/utopia/speculation/way_predictor_enabled", true);
		spec.lastway_table_entries = Sim()->getCfg()->getIntDefault("perf_model/utopia/speculation/lastway_table_entries", 8192);
		spec.enable_instruction_speculation = Sim()->getCfg()->getBoolDefault("perf_model/utopia/speculation/enable_instruction_speculation", false);
		if (spec.enabled)
		{
			spec.conf_table.resize(spec.conf_table_entries);
			spec.way_table.resize(spec.lastway_table_entries);
			if (core->getId() == 0)
				std::cout << "[MMU::Utopia] CATS speculation enabled (prefetch-only): wmax=" << spec.wmax
				          << " conf_threshold=" << spec.conf_threshold << "/" << ((1 << spec.conf_bits) - 1) << std::endl;
		}

		// Cost-based Top-K migration: the scoring parameters live in the allocator
		cost_topk_migration_enabled = false;
		cost_score_bits = 10;
		cost_ptw_base_inc = 1;
		cost_dram_inc_cap = 3;
		if (m_utopia && m_utopia->isCostTopKEnabled())
		{
			cost_topk_migration_enabled = true;
			const auto &cfg = m_utopia->getCostTopKConfig();
			cost_score_bits = cfg.score_bits;
			cost_ptw_base_inc = cfg.ptw_base_inc;
			cost_dram_inc_cap = cfg.dram_inc_cap;
			if (core->getId() == 0)
				std::cout << "[MMU::Utopia] Cost-based Top-K migration enabled: score_bits=" << cost_score_bits
				          << " ptw_base_inc=" << cost_ptw_base_inc << " dram_inc_cap=" << cost_dram_inc_cap << std::endl;
		}

		// Radix way table lookup instead of FPA + TAR
		radix_lookup_enabled = Sim()->getCfg()->getBoolDefault("perf_model/utopia/radix_lookup_enabled", true);

		// Metadata reads past the dedicated caches (then tagged PAGE_TABLE_DATA)
		bool disable_both = Sim()->getCfg()->getBoolDefault("perf_model/utopia/mmu/disable_fp_tar_caches", false);
		disable_fp_cache = disable_both || Sim()->getCfg()->getBoolDefault("perf_model/utopia/mmu/disable_fp_cache", false);
		disable_tar_cache = disable_both || Sim()->getCfg()->getBoolDefault("perf_model/utopia/mmu/disable_tar_cache", false);

		if (core->getId() == 0)
			std::cout << "[MMU::Utopia] RestSeg walk: " << (radix_lookup_enabled ? "radix way table" : "FPA + TAR")
			          << ", FP cache " << (disable_fp_cache ? "bypassed" : "on") << ", TAR cache " << (disable_tar_cache ? "bypassed" : "on")
			          << ", adaptive RSW " << (adaptive_rsw_enabled ? "on" : "off") << std::endl;
	}

	void MemoryManagementUnitUtopia::instantiateUTLB()
	{
		utlb = nullptr;
		utlb_enabled = Sim()->getCfg()->getBoolDefault("perf_model/utopia/utlb/enabled", true);
		if (!utlb_enabled)
			return;
		int utlb_entries = Sim()->getCfg()->getIntDefault("perf_model/utopia/utlb/entries", 2048);
		int utlb_assoc = Sim()->getCfg()->getIntDefault("perf_model/utopia/utlb/assoc", 8);
		int utlb_latency = Sim()->getCfg()->getIntDefault("perf_model/utopia/utlb/latency", 1);
		ComponentLatency access_latency(core->getDvfsDomain(), utlb_latency);
		utlb = new UtopiaTLB("utlb", core->getId(), utlb_entries, utlb_assoc, access_latency.getLatency());
	}

// ============================================================================
// Statistics
// ============================================================================

	void MemoryManagementUnitUtopia::registerMMUStats()
	{
		translation_stats = TranslationStats();
		sanity_checks_enabled = Sim()->getCfg()->getBoolDefault("perf_model/mmu/sanity_checks_enabled", false);
		sanity_check_violations = 0;
		registerStatsMetric(name, core->getId(), "sanity_check_violations", &sanity_check_violations);

		registerStatsMetric(name, core->getId(), "num_translations", &translation_stats.num_translations);
		registerStatsMetric(name, core->getId(), "num_translations_instruction", &translation_stats.num_translations_instruction);
		registerStatsMetric(name, core->getId(), "num_translations_data", &translation_stats.num_translations_data);
		registerStatsMetric(name, core->getId(), "page_faults", &translation_stats.page_faults);
		registerStatsMetric(name, core->getId(), "page_faults_instruction", &translation_stats.page_faults_instruction);
		registerStatsMetric(name, core->getId(), "page_faults_data", &translation_stats.page_faults_data);
		registerStatsMetric(name, core->getId(), "page_table_walks", &translation_stats.page_table_walks);
		registerStatsMetric(name, core->getId(), "l2_tlb_misses", &translation_stats.l2_tlb_misses);
		registerStatsMetric(name, core->getId(), "tlb_hits_instruction", &translation_stats.tlb_hits_instruction);
		registerStatsMetric(name, core->getId(), "tlb_hits_data", &translation_stats.tlb_hits_data);
		registerStatsMetric(name, core->getId(), "tlb_misses_instruction", &translation_stats.tlb_misses_instruction);
		registerStatsMetric(name, core->getId(), "tlb_misses_data", &translation_stats.tlb_misses_data);
		registerStatsMetric(name, core->getId(), "total_fault_latency", &translation_stats.total_fault_latency);
		registerStatsMetric(name, core->getId(), "total_walk_latency", &translation_stats.total_walk_latency);
		registerStatsMetric(name, core->getId(), "total_table_walk_latency", &translation_stats.total_walk_latency);
		registerStatsMetric(name, core->getId(), "total_rsw_latency", &translation_stats.total_rsw_latency);
		registerStatsMetric(name, core->getId(), "total_tlb_latency", &translation_stats.total_tlb_latency);
		registerStatsMetric(name, core->getId(), "total_walk_latency_flexseg_no_fault", &translation_stats.total_walk_latency_flexseg_no_fault);
		registerStatsMetric(name, core->getId(), "total_rsw_latency_restseg_no_fault", &translation_stats.total_rsw_latency_restseg_no_fault);
		registerStatsMetric(name, core->getId(), "total_walk_latency_flexseg", &translation_stats.total_walk_latency_flexseg);
		registerStatsMetric(name, core->getId(), "total_rsw_latency_restseg", &translation_stats.total_rsw_latency_restseg);
		registerStatsMetric(name, core->getId(), "total_tlb_latency_on_tlb_hit", &translation_stats.total_tlb_latency_on_tlb_hit);
		registerStatsMetric(name, core->getId(), "total_utlb_latency", &translation_stats.total_utlb_latency);
		registerStatsMetric(name, core->getId(), "total_translation_latency", &translation_stats.total_translation_latency);
		registerStatsMetric(name, core->getId(), "data_in_tlb", &translation_stats.data_in_tlb);
		registerStatsMetric(name, core->getId(), "data_in_utlb", &translation_stats.data_in_utlb);
		registerStatsMetric(name, core->getId(), "data_in_restseg", &translation_stats.data_in_restseg);
		registerStatsMetric(name, core->getId(), "data_in_flexseg", &translation_stats.data_in_flexseg);
		registerStatsMetric(name, core->getId(), "data_in_restseg_no_fault", &translation_stats.data_in_restseg_no_fault);
		registerStatsMetric(name, core->getId(), "data_in_flexseg_no_fault", &translation_stats.data_in_flexseg_no_fault);
		registerStatsMetric(name, core->getId(), "total_rsw_memory_requests", &translation_stats.total_rsw_memory_requests);
		registerStatsMetric(name, core->getId(), "flextorest_migrations", &translation_stats.flextorest_migrations);
		registerStatsMetric(name, core->getId(), "tlb_shootdowns", &translation_stats.tlb_shootdowns);
		registerStatsMetric(name, core->getId(), "requests_affected_by_migration", &translation_stats.requests_affected_by_migration);
		registerStatsMetric(name, core->getId(), "migration_stall_cycles", &translation_stats.migration_stall_cycles);
		registerStatsMetric(name, core->getId(), "rsw_sequential_accesses", &translation_stats.rsw_sequential_accesses);
		registerStatsMetric(name, core->getId(), "rsw_parallel_accesses", &translation_stats.rsw_parallel_accesses);
		registerStatsMetric(name, core->getId(), "meta_accesses_routed_to_ptw", &translation_stats.meta_accesses_routed_to_ptw);
		registerStatsMetric(name, core->getId(), "meta_accesses_routed_to_fp_tar", &translation_stats.meta_accesses_routed_to_fp_tar);
		registerRadixWalkCounters("radix", translation_stats.radix);
		registerStatsMetric(name, core->getId(), "cats_spec_attempts", &translation_stats.cats_spec_attempts);
		registerStatsMetric(name, core->getId(), "cats_spec_correct", &translation_stats.cats_spec_correct);
		registerStatsMetric(name, core->getId(), "cats_spec_wrong_way", &translation_stats.cats_spec_wrong_way);
		registerStatsMetric(name, core->getId(), "cats_spec_miss", &translation_stats.cats_spec_miss);
		registerStatsMetric(name, core->getId(), "cats_gated_ambiguity", &translation_stats.cats_gated_ambiguity);
		registerStatsMetric(name, core->getId(), "cats_gated_confidence", &translation_stats.cats_gated_confidence);
		registerStatsMetric(name, core->getId(), "cats_gated_instruction", &translation_stats.cats_gated_instruction);
		registerStatsMetric(name, core->getId(), "cats_prefetch_issued", &translation_stats.cats_prefetch_issued);
		registerStatsMetric(name, core->getId(), "cats_conf_updates", &translation_stats.cats_conf_updates);
		registerStatsMetric(name, core->getId(), "cats_way_predictions", &translation_stats.cats_way_predictions);
		registerStatsMetric(name, core->getId(), "cats_way_prediction_hits", &translation_stats.cats_way_prediction_hits);
		registerStatsMetric(name, core->getId(), "cats_considered", &translation_stats.cats_considered);
		registerStatsMetric(name, core->getId(), "cats_cold_spec_issued", &translation_stats.cats_cold_spec_issued);
		registerStatsMetric(name, core->getId(), "cost_topk_score_updates", &translation_stats.cost_topk_score_updates);
		registerStatsMetric(name, core->getId(), "cost_topk_migration_attempts", &translation_stats.cost_topk_migration_attempts);
		registerStatsMetric(name, core->getId(), "cost_topk_migrations_issued", &translation_stats.cost_topk_migrations_issued);

		rs_stats.resize(num_restsegs);
		for (int i = 0; i < num_restsegs; i++)
		{
			RestSegStats &s = rs_stats[i];
			s = RestSegStats();
			String sfx = "_restseg" + itostr(i);
			registerStatsMetric(name, core->getId(), "rsw_hits" + sfx, &s.rsw_hits);
			registerStatsMetric(name, core->getId(), "fpa_latency" + sfx, &s.fpa_latency);
			registerStatsMetric(name, core->getId(), "tar_latency" + sfx, &s.tar_latency);
			registerStatsMetric(name, core->getId(), "radix_latency" + sfx, &s.radix_latency);
			registerStatsMetric(name, core->getId(), "fpa_accesses" + sfx, &s.fpa_accesses);
			registerStatsMetric(name, core->getId(), "tar_accesses" + sfx, &s.tar_accesses);
			registerStatsMetric(name, core->getId(), "fpa_cache_hits" + sfx, &s.fpa_cache_hits);
			registerStatsMetric(name, core->getId(), "tar_cache_hits" + sfx, &s.tar_cache_hits);
			registerStatsMetric(name, core->getId(), "fp_filtered" + sfx, &s.fp_filtered);
			registerStatsMetric(name, core->getId(), "fpa_hit_l2" + sfx, &s.fpa_hit_l2);
			registerStatsMetric(name, core->getId(), "fpa_hit_nuca" + sfx, &s.fpa_hit_nuca);
			registerStatsMetric(name, core->getId(), "fpa_hit_dram" + sfx, &s.fpa_hit_dram);
			registerStatsMetric(name, core->getId(), "tar_hit_l2" + sfx, &s.tar_hit_l2);
			registerStatsMetric(name, core->getId(), "tar_hit_nuca" + sfx, &s.tar_hit_nuca);
			registerStatsMetric(name, core->getId(), "tar_hit_dram" + sfx, &s.tar_hit_dram);
			registerStatsMetric(name, core->getId(), "radix_lookups" + sfx, &s.radix_lookups);
			registerStatsMetric(name, core->getId(), "radix_hits" + sfx, &s.radix_hits);
			registerStatsMetric(name, core->getId(), "radix_misses" + sfx, &s.radix_misses);
		}

		translation_stats.tlb_latency_per_level = new SubsecondTime[tlb_subsystem->getTLBSubsystem().size()];
		for (UInt32 i = 0; i < tlb_subsystem->getTLBSubsystem().size(); i++)
			registerStatsMetric(name, core->getId(), "tlb_latency_" + itostr(i), &translation_stats.tlb_latency_per_level[i]);
	}

	void MemoryManagementUnitUtopia::registerRadixWalkCounters(const String &prefix, RadixWalkCounters &c)
	{
		c = RadixWalkCounters();
		registerStatsMetric(name, core->getId(), prefix + "_pwc_hits", &c.pwc_hits);
		registerStatsMetric(name, core->getId(), prefix + "_pwc_misses", &c.pwc_misses);
		registerStatsMetric(name, core->getId(), prefix + "_internal_hit_l2", &c.internal_hit_l2);
		registerStatsMetric(name, core->getId(), prefix + "_internal_hit_nuca", &c.internal_hit_nuca);
		registerStatsMetric(name, core->getId(), prefix + "_internal_hit_dram", &c.internal_hit_dram);
		registerStatsMetric(name, core->getId(), prefix + "_internal_accesses", &c.internal_accesses);
		registerStatsMetric(name, core->getId(), prefix + "_leaf_hit_l2", &c.leaf_hit_l2);
		registerStatsMetric(name, core->getId(), prefix + "_leaf_hit_nuca", &c.leaf_hit_nuca);
		registerStatsMetric(name, core->getId(), prefix + "_leaf_hit_dram", &c.leaf_hit_dram);
		registerStatsMetric(name, core->getId(), prefix + "_leaf_accesses", &c.leaf_accesses);
	}

	SubsecondTime MemoryManagementUnitUtopia::chargeRadixWalk(const std::vector<std::pair<IntPtr, bool>> &level_addresses, SubsecondTime start,
	                                                          IntPtr eip, Core::lock_signal_t lock, bool modeled, bool count, bool instruction,
	                                                          RadixWalkCounters &c)
	{
		SubsecondTime radix_latency = SubsecondTime::Zero();
		SubsecondTime current_time = start;
		for (size_t level_idx = 0; level_idx < level_addresses.size(); level_idx++)
		{
			IntPtr level_addr = level_addresses[level_idx].first;
			bool is_leaf = level_addresses[level_idx].second;
			bool pwc_hit = ptw_filter->lookupPWC(level_addr, current_time, level_idx, count);
			if (count)
			{
				if (pwc_hit) c.pwc_hits++; else c.pwc_misses++;
			}
			if (pwc_hit)
				continue;
			translationPacket packet;
			packet.eip = eip;
			packet.address = level_addr;
			packet.instruction = instruction;
			packet.lock_signal = lock;
			packet.modeled = modeled;
			packet.count = count;
			packet.type = is_leaf ? CacheBlockInfo::block_type_t::UTOPIA_RADIX_LEAF : CacheBlockInfo::block_type_t::UTOPIA_RADIX_INTERNAL;
			HitWhere::where_t hit_where;
			SubsecondTime level_latency = accessCache(packet, current_time, false, hit_where);
			radix_latency += level_latency;
			current_time = current_time + level_latency;
			if (count)
			{
				translation_stats.total_rsw_memory_requests++;
				if (is_leaf)
				{
					c.leaf_accesses++;
					countHitWhere(hit_where, c.leaf_hit_l2, c.leaf_hit_nuca, c.leaf_hit_dram);
				}
				else
				{
					c.internal_accesses++;
					countHitWhere(hit_where, c.internal_hit_l2, c.internal_hit_nuca, c.internal_hit_dram);
				}
			}
		}
		return radix_latency;
	}

	void MemoryManagementUnitUtopia::countHitWhere(HitWhere::where_t where, UInt64 &l2, UInt64 &nuca, UInt64 &dram)
	{
		if (where == HitWhere::L2_OWN)
			l2++;
		else if (where == HitWhere::NUCA_CACHE)
			nuca++;
		else if (where == HitWhere::DRAM || where == HitWhere::DRAM_LOCAL || where == HitWhere::DRAM_REMOTE)
			dram++;
	}

// ============================================================================
// Address translation
// ============================================================================

	IntPtr MemoryManagementUnitUtopia::performAddressTranslation(IntPtr eip, IntPtr address, bool instruction, Core::lock_signal_t lock, bool modeled, bool count)
	{
		dram_accesses_during_last_walk = 0;
		if (count)
		{
			translation_stats.num_translations++;
			if (instruction)
				translation_stats.num_translations_instruction++;
			else
				translation_stats.num_translations_data++;
		}
		SubsecondTime prev_total_translation_latency = translation_stats.total_translation_latency;
		IntPtr ppn_result = 0;
		int page_size = -1;
		int app_id = core->getThread() ? core->getThread()->getAppId() : 0;
		PageTable *page_table = Sim()->getMimicOS()->getPageTable(app_id);

		mmu_log->debug("Translation: VA=", SimLog::hex(address), instruction ? " (instruction)" : " (data)");

		// A page being migrated stalls its accesses until the migration completes
		UInt64 vpn_4kb_check = address >> 12;
		auto mig_it = migration_queue.find(vpn_4kb_check);
		if (mig_it != migration_queue.end())
		{
			SubsecondTime now = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);
			if (mig_it->second > now)
			{
				SubsecondTime migration_latency = mig_it->second - now;
				translation_stats.migration_stall_cycles += migration_latency;
				if (count)
				{
					translation_stats.requests_affected_by_migration++;
					translation_stats.total_translation_latency += migration_latency;
				}
			}
			else
				migration_queue.erase(mig_it);
		}

		// ------------------------------------------------------------------
		// TLB hierarchy lookup
		// ------------------------------------------------------------------
		const TLBSubsystem &tlbs = tlb_subsystem->getTLBSubsystem();
		SubsecondTime time = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);
		pt_walkers->removeCompletedEntries(time);

		bool hit = false;
		TLB *hit_tlb = NULL;
		CacheBlockInfo *tlb_block_info_hit = NULL;
		int hit_level = -1;
		for (UInt32 i = 0; i < tlbs.size() && !hit; i++)
		{
			for (UInt32 j = 0; j < tlbs[i].size(); j++)
			{
				bool tlb_stores_instructions = (tlbs[i][j]->getType() == TLBtype::Instruction) || (tlbs[i][j]->getType() == TLBtype::Unified);
				bool tlb_stores_data = !(tlbs[i][j]->getType() == TLBtype::Instruction);
				if (!((instruction && tlb_stores_instructions) || (!instruction && tlb_stores_data)))
					continue;
				CacheBlockInfo *tlb_block_info = tlbs[i][j]->lookup(address, time, count, lock, eip, modeled, count, page_table, instruction);
				if (tlb_block_info != NULL)
				{
					tlb_block_info_hit = tlb_block_info;
					hit_tlb = tlbs[i][j];
					hit_level = i;
					hit = true;
				}
			}
		}

		SubsecondTime charged_tlb_latency = SubsecondTime::Zero();
		if (hit)
		{
			// The levels before the hit, in series, the slowest TLB of each;
			// then the TLB that hit
			const TLBSubsystem &tlb_path = instruction ? tlb_subsystem->getInstructionPath() : tlb_subsystem->getDataPath();
			for (int i = 0; i < hit_level; i++)
			{
				SubsecondTime level_latency = SubsecondTime::Zero();
				for (UInt32 j = 0; j < tlb_path[i].size(); j++)
					level_latency = max(tlb_path[i][j]->getLatency(), level_latency);
				translation_stats.total_tlb_latency += level_latency;
				translation_stats.tlb_latency_per_level[i] += level_latency;
				charged_tlb_latency += level_latency;
			}
			translation_stats.total_tlb_latency += hit_tlb->getLatency();
			translation_stats.tlb_latency_per_level[hit_level] += hit_tlb->getLatency();
			charged_tlb_latency += hit_tlb->getLatency();
			shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time + charged_tlb_latency);
			if (count)
			{
				translation_stats.total_translation_latency += charged_tlb_latency;
				translation_stats.total_tlb_latency_on_tlb_hit += charged_tlb_latency;
				translation_stats.data_in_tlb++;
				if (instruction) translation_stats.tlb_hits_instruction++; else translation_stats.tlb_hits_data++;
			}
			page_size = tlb_block_info_hit->getPageSize();
			ppn_result = tlb_block_info_hit->getPPN();
			mmu_log->debug("TLB hit at level ", hit_level, ": page_size=", page_size, " ppn=", ppn_result);
		}
		else
		{
			for (UInt32 i = 0; i < tlbs.size(); i++)
			{
				SubsecondTime level_latency = SubsecondTime::Zero();
				for (UInt32 j = 0; j < tlbs[i].size(); j++)
					level_latency = max(tlbs[i][j]->getLatency(), level_latency);
				translation_stats.total_tlb_latency += level_latency;
				charged_tlb_latency += level_latency;
			}
			if (count)
			{
				translation_stats.l2_tlb_misses++;
				if (instruction) translation_stats.tlb_misses_instruction++; else translation_stats.tlb_misses_data++;
			}
			mmu_log->debug("TLB miss");
		}

		// ------------------------------------------------------------------
		// On a TLB miss: the UTLB and the RestSeg walk, overlapped with the
		// TLB lookup.  The RSW is performed even when the UTLB hits, to model
		// the parallel access; its result is used when the UTLB misses.
		// ------------------------------------------------------------------
		bool utlb_hit = false;
		SubsecondTime utlb_latency = SubsecondTime::Zero();
		uint8_t utlb_seg_id = 0;
		uint8_t utlb_way_idx = 0;
		int utlb_page_size = -1;
		bool rsw_hit = false;
		SubsecondTime rsw_latency = SubsecondTime::Zero();
		IntPtr rsw_ppn_result = 0;
		int rsw_page_size = -1;

		if (!hit && num_restsegs > 0)
		{
			if (utlb)
			{
				UTLBLookupResult utlb_result = utlb->lookup(address, 21, count);
				if (!utlb_result.hit)
					utlb_result = utlb->lookup(address, 12, count);
				if (utlb_result.hit)
				{
					utlb_hit = true;
					utlb_seg_id = utlb_result.seg_id;
					utlb_way_idx = utlb_result.way_idx;
					utlb_page_size = utlb_result.page_size;
					utlb_latency = utlb->getLatency();
					if (count)
					{
						translation_stats.data_in_utlb++;
						translation_stats.total_utlb_latency += utlb_latency;
					}
				}
			}

			// CATS: prefetch the candidate ways' data lines before the metadata answers
			RSWLookupResult cats_lookup;
			SpecDecision cats_decision;
			bool cats_ran = false;
			SubsecondTime cats_t_start = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);
			if (spec.enabled && !instruction)
			{
				cats_lookup = functionalRSWLookup(address);
				cats_ran = true;
				if (cats_lookup.num_candidates > 0)
				{
					if (count)
						translation_stats.cats_considered++;
					cats_decision = decideSpeculation(cats_lookup, instruction, count);
					if (cats_decision.do_speculate)
						executeSpeculation(cats_decision, eip, cats_t_start, count);
				}
			}

			auto rsw_result = RestSegWalk(address, instruction, eip, lock, modeled, count);
			rsw_page_size = get<0>(rsw_result);
			rsw_hit = (rsw_page_size != -1);
			rsw_ppn_result = get<1>(rsw_result);
			rsw_latency = get<2>(rsw_result);
			if (count)
				translation_stats.total_rsw_latency += rsw_latency;

			if (spec.enabled && cats_ran && cats_lookup.num_candidates > 0)
			{
				bool metadata_fast = (rsw_latency.getNS() < 10);
				int correct_way = -1;
				if (rsw_hit)
					for (int i = 0; i < num_restsegs; i++)
					{
						RestSeg *restseg = m_utopia->getRestSeg(i);
						int w = restseg->lookupWay(address, app_id);
						if (w >= 0)
						{
							correct_way = w;
							break;
						}
					}
				updateCATSOutcome(cats_lookup, cats_decision, rsw_hit, correct_way, metadata_fast, count);
			}
		}

		// A UTLB hit: the physical address from the RestSeg and the entry's
		// way -- the region's 2MB page when the entry is a coalesced region
		// of a 4KB RestSeg (step 24), the page's own frame otherwise.  An
		// entry whose page is no longer in that way (a migration eviction the
		// shootdown did not reach) is dropped and the RSW result stands.
		if (!hit && utlb_hit)
		{
			RestSeg *restseg = m_utopia->getRestSeg(utlb_seg_id);
			IntPtr ppn = (IntPtr)-1;
			if (restseg && restseg->lookupWay(address, app_id) == (int)utlb_way_idx)
			{
				if (utlb_page_size > restseg->getPageSizeBits())
					ppn = restseg->coalescedRegionPPN(address & ~((IntPtr(1) << utlb_page_size) - 1), utlb_way_idx);
				else
					ppn = restseg->calculatePPNFromWay(address, utlb_way_idx);
			}
			if (ppn != (IntPtr)-1)
			{
				ppn_result = ppn;
				page_size = utlb_page_size;
				rsw_hit = true;
				SubsecondTime lat = max(charged_tlb_latency, utlb_latency);
				if (count)
					translation_stats.total_translation_latency += lat;
				shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time + lat);
				mmu_log->debug("UTLB hit: seg=", (int)utlb_seg_id, " ppn=", ppn_result);
			}
			else
			{
				utlb->invalidate(address, utlb_page_size);
				utlb_hit = false;
			}
		}

		// An RSW hit without the UTLB: install the translation in the UTLB
		if (!hit && rsw_hit && !utlb_hit)
		{
			ppn_result = rsw_ppn_result;
			page_size = rsw_page_size;
			SubsecondTime lat = max(charged_tlb_latency, rsw_latency);
			if (count)
			{
				translation_stats.total_translation_latency += lat;
				translation_stats.total_rsw_latency_restseg_no_fault += rsw_latency;
				translation_stats.data_in_restseg_no_fault++;
			}
			shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time + lat);
			if (utlb)
				for (int seg_id = 0; seg_id < num_restsegs; seg_id++)
				{
					RestSeg *restseg = m_utopia->getRestSeg(seg_id);
					int way_idx = restseg->lookupWay(address, app_id);
					if (way_idx >= 0)
					{
						utlb->allocate(address, page_size, seg_id, way_idx, count);
						break;
					}
				}
			mmu_log->debug("RSW hit: page_size=", page_size, " ppn=", ppn_result);
		}

		// ------------------------------------------------------------------
		// TLB miss and RSW miss: the page table walk.  A fault goes to the
		// Utopia handler, after which the page is in a RestSeg (walked with
		// the RSW) or in the FlexSeg (walked again, now mapped).
		// ------------------------------------------------------------------
		SubsecondTime total_walk_latency = SubsecondTime::Zero();
		SubsecondTime total_fault_latency = SubsecondTime::Zero();
		bool was_serviced_by_flexseg = false;
		if (!hit && !rsw_hit)
		{
			shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time + charged_tlb_latency);
			if (count)
			{
				translation_stats.page_table_walks++;
				translation_stats.total_translation_latency += charged_tlb_latency;
			}
			SubsecondTime time_for_pt = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);
			struct MSHREntry pt_walker_entry;
			pt_walker_entry.request_time = time_for_pt;
			SubsecondTime delay = pt_walkers->getSlotAllocationDelay(pt_walker_entry.request_time);
			if (count)
				translation_stats.total_translation_latency += delay;
			shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, time_for_pt + delay);

			// Cold speculation: the fingerprint candidates of an RSW miss,
			// prefetched while the walk runs
			if (spec.enabled && !instruction)
			{
				RSWLookupResult rsw_lookup = functionalRSWLookup(address);
				if (rsw_lookup.num_candidates > 0)
				{
					SubsecondTime cold_spec_time = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);
					if (count)
						translation_stats.cats_considered++;
					SpecDecision cold_decision = decideSpeculation(rsw_lookup, instruction, count);
					if (cold_decision.do_speculate)
					{
						executeSpeculation(cold_decision, eip, cold_spec_time, count);
						if (count)
							translation_stats.cats_cold_spec_issued++;
						updateCATSOutcome(rsw_lookup, cold_decision, false, -1, false, count);
					}
				}
			}

			bool walk_fault = false;
			int walk_page_size = 0;
			IntPtr walk_ppn = 0;
			int walk_requested_frames = 0;
			total_walk_latency = walkFlexSeg(address, count, modeled, eip, lock, page_table, instruction,
			                                 walk_page_size, walk_ppn, walk_fault, walk_requested_frames);
			if (!walk_fault)
			{
				was_serviced_by_flexseg = true;
				page_size = walk_page_size;
				ppn_result = walk_ppn;
				if (count)
				{
					translation_stats.total_walk_latency += total_walk_latency;
					translation_stats.total_translation_latency += total_walk_latency;
					translation_stats.total_walk_latency_flexseg_no_fault += total_walk_latency;
					translation_stats.data_in_flexseg_no_fault++;
				}
				pt_walker_entry.completion_time = time_for_pt + delay + total_walk_latency;
				pt_walkers->allocate(pt_walker_entry);
				shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, pt_walker_entry.completion_time);
				mmu_log->debug("Walk: page_size=", page_size, " ppn=", ppn_result);
			}
			else
			{
				mmu_log->debug("Page fault at VA=", SimLog::hex(address));
				if (count)
				{
					translation_stats.page_faults++;
					if (instruction)
						translation_stats.page_faults_instruction++;
					else
						translation_stats.page_faults_data++;
				}
				// The fault, through the core's handler with a generic context
				ExceptionHandlerBase *handler = core->getExceptionHandler();
				ExceptionHandlerBase::FaultCtx fault_ctx{};
				fault_ctx.vpn = address >> 12;
				fault_ctx.page_table = page_table;
				fault_ctx.alloc_in.metadata_frames = walk_requested_frames;
				fault_ctx.alloc_in.is_instruction = instruction;
				handler->handle_page_fault(fault_ctx);

				total_fault_latency = Sim()->getMimicOS()->getPageFaultLatency();
				if (count)
					translation_stats.total_fault_latency += total_fault_latency;

				bool page_in_restseg = m_utopia && m_utopia->getLastAllocatedInRestSeg();
				if (page_in_restseg)
				{
					auto rsw_result = RestSegWalk(address, instruction, eip, lock, modeled, count);
					page_size = get<0>(rsw_result);
					ppn_result = get<1>(rsw_result);
					total_walk_latency = get<2>(rsw_result);
					if (page_size == -1)
					{
						std::cerr << "[MMU::Utopia] The page the fault handler placed in a RestSeg is not found there: VA=0x" << std::hex << address << std::dec << std::endl;
						abort();
					}
					if (count)
					{
						translation_stats.total_rsw_latency += total_walk_latency;
						translation_stats.total_translation_latency += total_walk_latency;
						translation_stats.total_rsw_latency_restseg += total_walk_latency;
						translation_stats.data_in_restseg++;
					}
					if (utlb)
					{
						RestSeg *restseg = m_utopia->getRestSeg(0);
						for (int seg_id = 0; seg_id < num_restsegs; seg_id++)
						{
							restseg = m_utopia->getRestSeg(seg_id);
							int way_idx = restseg->lookupWay(address, app_id);
							if (way_idx >= 0)
							{
								utlb->allocate(address, page_size, seg_id, way_idx, count);
								break;
							}
						}
					}
					mmu_log->debug("Fault -> RestSeg: page_size=", page_size, " ppn=", ppn_result);
				}
				else
				{
					was_serviced_by_flexseg = true;
					bool fault2 = false;
					int ps2 = 0;
					IntPtr ppn2 = 0;
					int rf2 = 0;
					total_walk_latency = walkFlexSeg(address, count, modeled, eip, lock, page_table, instruction, ps2, ppn2, fault2, rf2);
					if (fault2)
					{
						std::cerr << "[MMU::Utopia] The walk after the fault handler faulted again: VA=0x" << std::hex << address << std::dec << std::endl;
						abort();
					}
					page_size = ps2;
					ppn_result = ppn2;
					if (count)
					{
						translation_stats.total_walk_latency += total_walk_latency;
						translation_stats.total_translation_latency += total_walk_latency;
						translation_stats.total_walk_latency_flexseg += total_walk_latency;
						translation_stats.data_in_flexseg++;
					}
					mmu_log->debug("Fault -> FlexSeg: page_size=", page_size, " ppn=", ppn_result);
				}
				pt_walker_entry.completion_time = time_for_pt + delay + total_walk_latency + total_fault_latency;
				pt_walkers->allocate(pt_walker_entry);
				PseudoInstruction *i = new PageFaultRoutineInstruction(total_fault_latency);
				getCore()->getPerformanceModel()->queuePseudoInstruction(i);
				shmem_perf_model->setElapsedTime(ShmemPerfModel::_USER_THREAD, pt_walker_entry.completion_time);
			}

			// --------------------------------------------------------------
			// Migration of a walked FlexSeg page.  The epochs of the cost
			// policy advance on walked translations only, so its budget
			// follows the walk load.
			// --------------------------------------------------------------
			if (was_serviced_by_flexseg && m_utopia)
			{
				UInt64 vpn_4kb = address >> 12;
				bool migrated_by_cost_policy = false;
				if (cost_topk_migration_enabled && page_size == 12)
				{
					int dram_cap = std::min(getDramAccessesDuringLastWalk(), cost_dram_inc_cap);
					UInt16 new_score = m_utopia->bumpCostScore(vpn_4kb, cost_ptw_base_inc + dram_cap);
					m_utopia->updateCostTopK(vpn_4kb, new_score, count);
					m_utopia->processEpochs(count);
					if (count)
					{
						translation_stats.cost_topk_score_updates++;
						translation_stats.cost_topk_migration_attempts++;
					}
					auto [migrated, new_ppn] = m_utopia->considerCostBasedMigration(address, ppn_result, page_size, app_id, vpn_4kb, count);
					if (migrated)
					{
						IntPtr old_ppn = ppn_result;
						ppn_result = new_ppn;
						migrated_by_cost_policy = true;
						completeMigration(address, old_ppn, new_ppn, page_size, vpn_4kb, count);
						if (count)
							translation_stats.cost_topk_migrations_issued++;
					}
				}
				if (!migrated_by_cost_policy && legacy_migration_enabled &&
				    m_utopia->getMigrationPolicyType() == UtopiaAlloc::MigrationPolicyType::None)
				{
					auto &st = ptw_stats[vpn_4kb];
					st.first++;
					st.second += getDramAccessesDuringLastWalk();
					if (st.first >= ptw_migration_threshold && st.second >= dram_accesses_migration_threshold)
					{
						IntPtr old_ppn = ppn_result;
						IntPtr new_ppn = m_utopia->migratePage(address, ppn_result, page_size, app_id);
						if (new_ppn != 0)
						{
							ppn_result = new_ppn;
							completeMigration(address, old_ppn, new_ppn, page_size, vpn_4kb, count);
						}
					}
				}
			}
		}

		// ------------------------------------------------------------------
		// TLB allocation, with the eviction cascade
		// ------------------------------------------------------------------
		const TLBSubsystem &alloc_tlbs = instruction ? tlb_subsystem->getInstructionPath() : tlb_subsystem->getDataPath();
		std::map<int, vector<EvictedTranslation>> evicted_translations;
		int tlb_levels = alloc_tlbs.size();
		for (int i = 0; i < tlb_levels; i++)
		{
			for (UInt32 j = 0; j < alloc_tlbs[i].size(); j++)
			{
				if (alloc_tlbs[i][j]->getPrefetch())
					continue;
				if ((i > 0) && (evicted_translations[i - 1].size() != 0))
				{
					for (UInt32 k = 0; k < evicted_translations[i - 1].size(); k++)
					{
						const EvictedTranslation &evicted = evicted_translations[i - 1][k];
						if (alloc_tlbs[i][j]->supportsPageSize(evicted.page_size))
						{
							TLBAllocResult result = alloc_tlbs[i][j]->allocate(evicted.address, time, count, lock, evicted.page_size, evicted.ppn, false, instruction);
							if (result.evicted)
								evicted_translations[i].emplace_back(result.address, result.page_size, result.ppn);
						}
					}
				}
				if (alloc_tlbs[i][j]->supportsPageSize(page_size) && alloc_tlbs[i][j]->getAllocateOnMiss() && (!hit || hit_level > i))
				{
					TLBAllocResult result = alloc_tlbs[i][j]->allocate(address, time, count, lock, page_size, ppn_result, false, instruction);
					if (result.evicted)
					{
						evicted_translations[i].emplace_back(result.address, result.page_size, result.ppn);
						onTLBLevelEviction(i, result.address, result.page_size, result.ppn, eip, lock, instruction);
					}
				}
			}
		}

		// ------------------------------------------------------------------
		// The physical address
		// ------------------------------------------------------------------
		constexpr IntPtr base_page_size_in_bytes = 1ULL << 12;
		IntPtr page_size_in_bytes = 1ULL << page_size;
		IntPtr offset = address & (page_size_in_bytes - 1);
		IntPtr final_physical_address = (ppn_result * base_page_size_in_bytes) + offset;

		if (count)
		{
			MimicOS *mimicos = Sim()->getMimicOS();
			if (mimicos && mimicos->isPerCoreStatsInitialized())
			{
				SubsecondTime this_translation_latency = translation_stats.total_translation_latency - prev_total_translation_latency;
				mimicos->updateTranslationStats(core->getId(), this_translation_latency, !hit, total_walk_latency);
			}
		}
		mmu_log->debug("Translation complete: VA=", SimLog::hex(address), " -> PA=", SimLog::hex(final_physical_address));

		if (sanity_checks_enabled)
		{
			IntPtr page_size_bytes = 1ULL << page_size;
			IntPtr va_page = address & ~(page_size_bytes - 1);
			IntPtr pa_page = final_physical_address & ~(page_size_bytes - 1);
			const char *path = hit ? "TLB_HIT" : utlb_hit ? "UTLB_HIT" : rsw_hit ? "RSW_HIT" : "PTW";
			auto va_it = va_to_pa_map.find(va_page);
			if (va_it != va_to_pa_map.end())
			{
				if (va_it->second != pa_page)
				{
					sanity_check_violations++;
					std::cerr << "[MMU SANITY ERROR] Core " << core->getId() << " VA=0x" << std::hex << va_page
					          << " mapped to different PAs! Previous=0x" << va_it->second << " Current=0x" << pa_page << std::dec
					          << " Path=" << path << " page_size=" << page_size << std::endl;
					assert(false && "VA-PA mapping inconsistency detected!");
				}
			}
			else
				va_to_pa_map[va_page] = pa_page;
			auto pa_it = pa_to_va_map.find(pa_page);
			if (pa_it != pa_to_va_map.end())
			{
				if (pa_it->second != va_page)
				{
					sanity_check_violations++;
					std::cerr << "[MMU SANITY ERROR] Core " << core->getId() << " PA=0x" << std::hex << pa_page
					          << " assigned to multiple VAs! Previous=0x" << pa_it->second << " Current=0x" << va_page << std::dec
					          << " Path=" << path << std::endl;
					assert(false && "PA assigned to multiple VAs detected!");
				}
			}
			else
				pa_to_va_map[pa_page] = va_page;
		}
		return final_physical_address;
	}

	// What follows a migration: the stale translations go, the sanity maps
	// move, and the page's next accesses stall for the migration's latency
	void MemoryManagementUnitUtopia::completeMigration(IntPtr address, IntPtr old_ppn, IntPtr new_ppn, int page_size, UInt64 vpn_4kb, bool count)
	{
		performTLBShootdown(address, page_size);
		if (m_utopia->didLastCauseEviction())
		{
			IntPtr evicted_va = m_utopia->getLastEvictedAddress();
			int evicted_page_size = m_utopia->getLastEvictedPageSizeBits();
			invalidateSanityCheckMaps(evicted_va, evicted_page_size);
			performTLBShootdown(evicted_va, evicted_page_size);
			if (sanity_checks_enabled)
			{
				int app_id = core->getThread() ? core->getThread()->getAppId() : 0;
				LOG_ASSERT_ERROR(!m_utopia->checkIsInRestSeg(evicted_va, app_id), "Evicted page 0x%lx still in a RestSeg", evicted_va);
				const TLBSubsystem &data_tlbs = tlb_subsystem->getDataPath();
				for (size_t level = 0; level < data_tlbs.size(); level++)
					for (size_t t = 0; t < data_tlbs[level].size(); t++)
						LOG_ASSERT_ERROR(!data_tlbs[level][t]->contains(evicted_va, evicted_page_size),
						                 "TLB shootdown failed for evicted page 0x%lx", evicted_va);
			}
		}
		if (sanity_checks_enabled)
		{
			int app_id = core->getThread() ? core->getThread()->getAppId() : 0;
			LOG_ASSERT_ERROR(m_utopia->checkIsInRestSeg(address, app_id), "Migrated page 0x%lx not in a RestSeg", address);
		}
		updateSanityCheckMaps(address, old_ppn, new_ppn, page_size);
		migration_queue[vpn_4kb] = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD) +
		                           ComponentLatency(core->getDvfsDomain(), 200).getLatency();
		if (count)
			translation_stats.flextorest_migrations++;
		mmu_log->debug("Migrated VA=", SimLog::hex(address), " ppn ", old_ppn, " -> ", new_ppn);
	}

// ============================================================================
// Walks
// ============================================================================

	SubsecondTime MemoryManagementUnitUtopia::walkFlexSeg(IntPtr address, bool count, bool modeled, IntPtr eip, Core::lock_signal_t lock,
	                                                      PageTable *page_table, bool instruction,
	                                                      int &out_page_size, IntPtr &out_ppn, bool &out_fault, int &out_requested_frames)
	{
		PTWResult ptw_result = page_table->initializeWalk(address, count, false /* is_prefetch */, false /* restart_walk */);
		out_fault = ptw_result.fault_happened;
		out_page_size = ptw_result.page_size;
		out_ppn = ptw_result.ppn;
		out_requested_frames = ptw_result.requested_frames;
		if (out_fault)
			return SubsecondTime::Zero();
		stats.memory_accesses_before_filtering += ptw_result.accesses.size();
		ptw_result = filterPTWResult(address, ptw_result, page_table, count);
		return calculatePTWCycles(ptw_result, count, modeled, eip, lock, address, instruction, false);
	}

	std::tuple<int, IntPtr, SubsecondTime> MemoryManagementUnitUtopia::RestSegWalk(IntPtr address, bool instruction, IntPtr eip, Core::lock_signal_t lock, bool modeled, bool count)
	{
		SubsecondTime t_start = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);
		IntPtr final_address = 0;
		int page_size = -1;
		int app_id = core->getThread() ? core->getThread()->getAppId() : 0;
		if (num_restsegs == 0)
			return make_tuple(page_size, final_address, SubsecondTime::Zero());

		// Sequential once one RestSeg dominates the hits, else parallel
		bool use_sequential = false;
		int first_restseg = 0;
		if (num_restsegs > 1 && adaptive_rsw_enabled && rsw_total_hits >= rsw_warmup_accesses)
		{
			rsw_dominant_restseg = -1;
			for (int i = 0; i < num_restsegs; i++)
				if ((double)rsw_hits_per_restseg[i] / rsw_total_hits >= rsw_sequential_threshold)
				{
					use_sequential = true;
					first_restseg = i;
					rsw_dominant_restseg = i;
					break;
				}
		}

		struct RestSegAccess
		{
			bool hit = false;
			bool fp_filtered = false;      // no fingerprint matched: the TAR read is squashed
			SubsecondTime fpa_latency = SubsecondTime::Zero();
			SubsecondTime tar_latency = SubsecondTime::Zero();
			int page_size_bits = 0;
			IntPtr physical_address = 0;
		};
		std::vector<RestSegAccess> rs_access(num_restsegs);

		// One metadata line through its dedicated cache or, bypassed, through
		// the hierarchy tagged as page-table data; else through the hierarchy
		// with its own block type
		auto readMetadata = [&](IntPtr line, bool bypass, UtopiaCache *cache, CacheBlockInfo::block_type_t type,
		                        SubsecondTime at, UInt64 &cache_hits, UInt64 &l2, UInt64 &nuca, UInt64 &dram) -> SubsecondTime {
			if (!bypass)
			{
				if (count) translation_stats.meta_accesses_routed_to_fp_tar++;
				if (cache->lookup(line, at, true, count) == UtopiaCache::HIT)
				{
					if (count) cache_hits++;
					return SubsecondTime::Zero();
				}
			}
			else if (count)
				translation_stats.meta_accesses_routed_to_ptw++;
			translationPacket packet;
			packet.eip = eip;
			packet.address = line;
			packet.instruction = instruction;
			packet.lock_signal = lock;
			packet.modeled = modeled;
			packet.count = count;
			packet.type = bypass ? CacheBlockInfo::block_type_t::PAGE_TABLE_DATA : type;
			HitWhere::where_t hit_where;
			SubsecondTime lat = accessCache(packet, at, false, hit_where);
			if (count)
			{
				translation_stats.total_rsw_memory_requests++;
				countHitWhere(hit_where, l2, nuca, dram);
			}
			return lat;
		};

		// FPA + TAR: both launched at access_start; the TAR read is squashed
		// when no fingerprint matches, else the critical path is the slower
		auto accessRestSegFpaTar = [&](int i, SubsecondTime access_start) -> SubsecondTime {
			RestSeg *restseg = m_utopia->getRestSeg(i);
			RestSegStats &s = rs_stats[i];
			rs_access[i].page_size_bits = restseg->getPageSizeBits();
			if (restseg->inRestSeg(address, app_id, count))
			{
				rs_access[i].hit = true;
				rs_access[i].physical_address = restseg->calculatePhysicalAddress(address, app_id);
			}
			if (count) s.fpa_accesses++;
			rs_access[i].fpa_latency = readMetadata(restseg->calculateFingerprintAddress(address, core->getId()), disable_fp_cache, fpa_cache,
			                                        CacheBlockInfo::block_type_t::UTOPIA_FP, access_start,
			                                        s.fpa_cache_hits, s.fpa_hit_l2, s.fpa_hit_nuca, s.fpa_hit_dram);
			if (count) s.fpa_latency += rs_access[i].fpa_latency;

			std::vector<int> matching_ways = restseg->findMatchingFingerprints(address, restseg->computeLookupFingerprint(address));
			rs_access[i].fp_filtered = matching_ways.empty();
			if (rs_access[i].fp_filtered)
			{
				if (count) s.fp_filtered++;
				return rs_access[i].fpa_latency;
			}
			if (count) s.tar_accesses++;
			rs_access[i].tar_latency = readMetadata(restseg->calculateTagAddress(address, core->getId()), disable_tar_cache, tar_cache,
			                                        CacheBlockInfo::block_type_t::UTOPIA_TAR, access_start,
			                                        s.tar_cache_hits, s.tar_hit_l2, s.tar_hit_nuca, s.tar_hit_dram);
			if (count) s.tar_latency += rs_access[i].tar_latency;
			return max(rs_access[i].fpa_latency, rs_access[i].tar_latency);
		};

		// Radix way table: one line per level in sequence, each through the
		// PWC first and the hierarchy on a miss
		auto accessRestSegRadix = [&](int i, SubsecondTime access_start) -> SubsecondTime {
			RestSeg *restseg = m_utopia->getRestSeg(i);
			RestSegStats &s = rs_stats[i];
			rs_access[i].page_size_bits = restseg->getPageSizeBits();
			int way_out = -1;
			bool radix_hit = restseg->lookupWayRadix(address, app_id, way_out);
			if (radix_hit && way_out >= 0)
			{
				rs_access[i].hit = true;
				rs_access[i].physical_address = restseg->calculatePPNFromWay(address, way_out);
				restseg->touchWayLRU(address, way_out);
			}
			if (count)
			{
				s.radix_lookups++;
				if (radix_hit) s.radix_hits++; else s.radix_misses++;
			}
			std::vector<std::pair<IntPtr, bool>> level_addresses;
			restseg->calculateRadixLevelAddresses(address, app_id, level_addresses);
			SubsecondTime radix_latency = chargeRadixWalk(level_addresses, access_start, eip, lock, modeled, count, instruction, translation_stats.radix);
			if (count) s.radix_latency += radix_latency;
			return radix_latency;
		};

		auto accessRestSeg = [&](int i, SubsecondTime access_start) -> SubsecondTime {
			return radix_lookup_enabled ? accessRestSegRadix(i, access_start) : accessRestSegFpaTar(i, access_start);
		};

		SubsecondTime result_latency = SubsecondTime::Zero();
		int hit_restseg = -1;
		if (num_restsegs == 1)
		{
			result_latency = accessRestSeg(0, t_start);
			if (rs_access[0].hit)
				hit_restseg = 0;
		}
		else if (use_sequential)
		{
			if (count)
				translation_stats.rsw_sequential_accesses++;
			SubsecondTime so_far = SubsecondTime::Zero();
			so_far = accessRestSeg(first_restseg, t_start);
			if (rs_access[first_restseg].hit)
				hit_restseg = first_restseg;
			else
				for (int i = 0; i < num_restsegs && hit_restseg < 0; i++)
				{
					if (i == first_restseg)
						continue;
					so_far += accessRestSeg(i, t_start + so_far);
					if (rs_access[i].hit)
						hit_restseg = i;
				}
			result_latency = so_far;
		}
		else
		{
			if (count)
				translation_stats.rsw_parallel_accesses++;
			std::vector<SubsecondTime> rs_lat(num_restsegs);
			for (int i = 0; i < num_restsegs; i++)
				rs_lat[i] = accessRestSeg(i, t_start);
			for (int i = 0; i < num_restsegs; i++)
				if (rs_access[i].hit)
				{
					hit_restseg = i;
					break;
				}
			// A hit completes when its RestSeg does; a miss waits for all
			if (hit_restseg >= 0)
				result_latency = rs_lat[hit_restseg];
			else
				for (int i = 0; i < num_restsegs; i++)
					result_latency = max(result_latency, rs_lat[i]);
		}

		if (hit_restseg >= 0)
		{
			page_size = rs_access[hit_restseg].page_size_bits;
			final_address = rs_access[hit_restseg].physical_address;
			rsw_total_hits++;
			rsw_hits_per_restseg[hit_restseg]++;
			if (count)
				rs_stats[hit_restseg].rsw_hits++;
		}
		return make_tuple(page_size, final_address, result_latency);
	}

	RSWLookupResult MemoryManagementUnitUtopia::functionalRSWLookup(IntPtr address)
	{
		RSWLookupResult result;
		result.virtual_address = address;
		int app_id = core->getThread() ? core->getThread()->getAppId() : 0;
		for (int i = 0; i < num_restsegs; i++)
		{
			RestSeg *restseg = m_utopia->getRestSeg(i);
			if (!restseg->inRestSeg(address, app_id, false))
				continue;
			result.hit = true;
			result.seg_id = i;
			result.way = restseg->getWayForAddress(address, app_id);
			result.page_size_bits = restseg->getPageSizeBits();
			result.ppn = restseg->calculatePhysicalAddress(address, app_id);
			result.physical_address = (result.ppn << 12) | (address & ((1ULL << result.page_size_bits) - 1));
			result.fpa_address = restseg->calculateFingerprintAddress(address, core->getId());
			result.tar_address = restseg->calculateTagAddress(address, core->getId());
			result.set_index = (address >> result.page_size_bits) % restseg->getNumSets();
			std::vector<int> matching_ways = restseg->findMatchingFingerprints(address, restseg->computeLookupFingerprint(address));
			result.num_candidates = std::min((int)matching_ways.size(), RSWLookupResult::MAX_CANDIDATES);
			for (int c = 0; c < result.num_candidates; c++)
			{
				result.candidate_ways[c] = matching_ways[c];
				result.candidate_pas[c] = restseg->calculatePhysicalAddressForWay(address, matching_ways[c]);
			}
			return result;
		}
		// A miss still has fingerprint candidates, for cold speculation: the
		// RestSeg with the most of them
		int best_seg = -1;
		std::vector<int> best_matching_ways;
		for (int i = 0; i < num_restsegs; i++)
		{
			RestSeg *restseg = m_utopia->getRestSeg(i);
			std::vector<int> matching_ways = restseg->findMatchingFingerprints(address, restseg->computeLookupFingerprint(address));
			if ((int)matching_ways.size() > (int)best_matching_ways.size())
			{
				best_seg = i;
				best_matching_ways = std::move(matching_ways);
			}
		}
		if (best_seg >= 0)
		{
			RestSeg *restseg = m_utopia->getRestSeg(best_seg);
			result.seg_id = best_seg;
			result.page_size_bits = restseg->getPageSizeBits();
			result.set_index = (address >> result.page_size_bits) % restseg->getNumSets();
			result.fpa_address = restseg->calculateFingerprintAddress(address, core->getId());
			result.tar_address = restseg->calculateTagAddress(address, core->getId());
			result.num_candidates = std::min((int)best_matching_ways.size(), RSWLookupResult::MAX_CANDIDATES);
			for (int c = 0; c < result.num_candidates; c++)
			{
				result.candidate_ways[c] = best_matching_ways[c];
				result.candidate_pas[c] = restseg->calculatePhysicalAddressForWay(address, best_matching_ways[c]);
			}
		}
		return result;
	}

// ============================================================================
// Shootdown and sanity maps
// ============================================================================

	void MemoryManagementUnitUtopia::performTLBShootdown(IntPtr address, int page_size)
	{
		translation_stats.tlb_shootdowns++;
		// Every TLB of both paths: a TLB may hold the page at a size it does
		// not officially support, and invalidate is a no-op when it does not
		const TLBSubsystem &data_tlbs = tlb_subsystem->getDataPath();
		for (size_t level = 0; level < data_tlbs.size(); level++)
			for (size_t t = 0; t < data_tlbs[level].size(); t++)
				data_tlbs[level][t]->invalidate(address, page_size);
		const TLBSubsystem &inst_tlbs = tlb_subsystem->getInstructionPath();
		for (size_t level = 0; level < inst_tlbs.size(); level++)
			for (size_t t = 0; t < inst_tlbs[level].size(); t++)
				inst_tlbs[level][t]->invalidate(address, page_size);
		if (utlb)
			utlb->invalidate(address, page_size);
		// The FPA and TAR lines of the page's set in every RestSeg
		for (int i = 0; i < num_restsegs; i++)
		{
			RestSeg *restseg = m_utopia->getRestSeg(i);
			fpa_cache->invalidate(restseg->calculateFingerprintAddress(address, core->getId()));
			tar_cache->invalidate(restseg->calculateTagAddress(address, core->getId()));
		}
	}

	void MemoryManagementUnitUtopia::updateSanityCheckMaps(IntPtr address, IntPtr old_ppn, IntPtr new_ppn, int page_size)
	{
		if (!sanity_checks_enabled)
			return;
		constexpr IntPtr base_page_size_in_bytes = 1ULL << 12;
		IntPtr page_size_bytes = 1ULL << page_size;
		IntPtr va_page = address & ~(page_size_bytes - 1);
		IntPtr offset = address & (page_size_bytes - 1);
		IntPtr old_pa_page = ((old_ppn * base_page_size_in_bytes) + offset) & ~(page_size_bytes - 1);
		IntPtr new_pa_page = ((new_ppn * base_page_size_in_bytes) + offset) & ~(page_size_bytes - 1);
		va_to_pa_map[va_page] = new_pa_page;
		pa_to_va_map.erase(old_pa_page);
		pa_to_va_map[new_pa_page] = va_page;
	}

	void MemoryManagementUnitUtopia::invalidateSanityCheckMaps(IntPtr address, int page_size)
	{
		if (!sanity_checks_enabled)
			return;
		IntPtr page_size_bytes = 1ULL << page_size;
		IntPtr va_page = address & ~(page_size_bytes - 1);
		auto va_it = va_to_pa_map.find(va_page);
		if (va_it != va_to_pa_map.end())
		{
			pa_to_va_map.erase(va_it->second);
			va_to_pa_map.erase(va_it);
		}
	}

	PTWResult MemoryManagementUnitUtopia::filterPTWResult(IntPtr virtual_address, PTWResult ptw_result, PageTable *page_table, bool count)
	{
		return ptw_filter->filterPTWResult(virtual_address, ptw_result, page_table, count);
	}

	void MemoryManagementUnitUtopia::discoverVMAs()
	{
	}

// ============================================================================
// CATS: confidence-aware throttled speculation
// ============================================================================

	UInt64 MemoryManagementUnitUtopia::computeSpecTableKey(int seg_id, int set_index)
	{
		return ((UInt64)seg_id << 16) ^ (UInt64)set_index;
	}

	MemoryManagementUnitUtopia::SpecCtrl::ConfEntry &MemoryManagementUnitUtopia::lookupConfEntry(UInt64 key)
	{
		UInt64 index = key % spec.conf_table.size();
		UInt16 tag = (UInt16)((key >> 16) & 0xFFFF);
		SpecCtrl::ConfEntry &entry = spec.conf_table[index];
		if (entry.tag != tag)
		{
			entry.tag = tag;
			entry.conf = 0;
		}
		return entry;
	}

	int MemoryManagementUnitUtopia::lookupWayPredictor(UInt64 key)
	{
		if (!spec.way_predictor_enabled)
			return -1;
		UInt64 index = key % spec.way_table.size();
		UInt16 tag = (UInt16)((key >> 16) & 0xFFFF);
		SpecCtrl::WayEntry &entry = spec.way_table[index];
		if (entry.tag == tag && entry.usefulness > 0)
			return entry.last_way;
		return -1;
	}

	// Speculate when the access is data (or instruction speculation is on),
	// the candidates are neither none nor too many, and the set's confidence
	// is high enough; the predicted way goes first, then another candidate
	SpecDecision MemoryManagementUnitUtopia::decideSpeculation(const RSWLookupResult &lr, bool instruction, bool count)
	{
		SpecDecision decision;
		if (!spec.enabled)
			return decision;
		if (instruction && !spec.enable_instruction_speculation)
		{
			if (count) translation_stats.cats_gated_instruction++;
			return decision;
		}
		if (lr.num_candidates == 0 || lr.num_candidates > spec.wmax)
		{
			if (count) translation_stats.cats_gated_ambiguity++;
			return decision;
		}
		UInt64 key = computeSpecTableKey(lr.seg_id, lr.set_index);
		SpecCtrl::ConfEntry &conf_entry = lookupConfEntry(key);
		if (conf_entry.conf < spec.conf_threshold)
		{
			if (count) translation_stats.cats_gated_confidence++;
			return decision;
		}
		if (count) translation_stats.cats_spec_attempts++;
		decision.do_speculate = true;

		int predicted_way_id = lookupWayPredictor(key);
		int predicted_idx = -1;
		if (predicted_way_id >= 0)
			for (int i = 0; i < lr.num_candidates; i++)
				if (lr.candidate_ways[i] == predicted_way_id)
				{
					predicted_idx = i;
					break;
				}
		if (predicted_idx >= 0)
		{
			decision.chosen_way0 = lr.candidate_ways[predicted_idx];
			decision.prefetch_addr0 = lr.candidate_pas[predicted_idx];
			if (count) translation_stats.cats_way_predictions++;
			for (int i = 0; i < lr.num_candidates; i++)
				if (i != predicted_idx)
				{
					decision.chosen_way1 = lr.candidate_ways[i];
					decision.prefetch_addr1 = lr.candidate_pas[i];
					break;
				}
		}
		else
		{
			decision.chosen_way0 = lr.candidate_ways[0];
			decision.prefetch_addr0 = lr.candidate_pas[0];
			if (lr.num_candidates >= 2)
			{
				decision.chosen_way1 = lr.candidate_ways[1];
				decision.prefetch_addr1 = lr.candidate_pas[1];
			}
		}
		return decision;
	}

	void MemoryManagementUnitUtopia::updateCATSOutcome(const RSWLookupResult &lr, const SpecDecision &decision, bool hit, int correct_way, bool metadata_fast, bool count)
	{
		if (!spec.enabled)
			return;
		UInt64 key = computeSpecTableKey(lr.seg_id, lr.set_index);
		SpecCtrl::ConfEntry &conf_entry = lookupConfEntry(key);
		int conf_max = (1 << spec.conf_bits) - 1;
		if (hit && decision.do_speculate)
		{
			if (correct_way == decision.chosen_way0 || correct_way == decision.chosen_way1)
			{
				if (conf_entry.conf < conf_max) conf_entry.conf++;
				if (count)
				{
					translation_stats.cats_spec_correct++;
					translation_stats.cats_way_prediction_hits++;
				}
			}
			else
			{
				if (conf_entry.conf > 0) conf_entry.conf--;
				if (count) translation_stats.cats_spec_wrong_way++;
			}
		}
		else if (!hit && decision.do_speculate)
		{
			if (conf_entry.conf > 0) conf_entry.conf--;
			if (count) translation_stats.cats_spec_miss++;
		}
		else if (hit && !decision.do_speculate && metadata_fast)
		{
			if (conf_entry.conf < conf_max) conf_entry.conf++;
		}
		if (count) translation_stats.cats_conf_updates++;

		if (spec.way_predictor_enabled && hit && correct_way >= 0)
		{
			UInt64 way_index = key % spec.way_table.size();
			UInt16 way_tag = (UInt16)((key >> 16) & 0xFFFF);
			SpecCtrl::WayEntry &way_entry = spec.way_table[way_index];
			if (way_entry.tag == way_tag)
			{
				way_entry.last_way = correct_way;
				if (way_entry.usefulness < 3) way_entry.usefulness++;
			}
			else if (way_entry.usefulness == 0)
			{
				way_entry.tag = way_tag;
				way_entry.last_way = correct_way;
				way_entry.usefulness = 1;
			}
			else
				way_entry.usefulness--;
		}
	}

	void MemoryManagementUnitUtopia::executeSpeculation(const SpecDecision &decision, IntPtr eip, SubsecondTime t_start, bool count)
	{
		if (!decision.do_speculate)
			return;
		MMUCacheInterface *l2 = memory_manager->getCacheCntlrAt(core->getId(), MemComponent::component_t::L2_CACHE);
		if (!l2)
			return;
		if (decision.prefetch_addr0 != 0)
		{
			l2->handleMMUPrefetch(eip, decision.prefetch_addr0 & ~((IntPtr)63), t_start, CacheBlockInfo::block_type_t::DATA);
			if (count) translation_stats.cats_prefetch_issued++;
		}
		if (decision.prefetch_addr1 != 0)
		{
			l2->handleMMUPrefetch(eip, decision.prefetch_addr1 & ~((IntPtr)63), t_start, CacheBlockInfo::block_type_t::DATA);
			if (count) translation_stats.cats_prefetch_issued++;
		}
	}

} // namespace ParametricDramDirectoryMSI
