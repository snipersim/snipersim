#pragma once

#include "tlb_prefetcher_base.h"
#include "stride_prefetcher.h"
#include "H2Prefetcher.h"
#include "arbitrary_stride_prefetcher.h"
#include "TemporalPTEPrefetcher.h"
#include "BertiTLBPrefetcher.h"

namespace ParametricDramDirectoryMSI
{
	class TLBprefetcherFactory
	{
	public:
		static TLBPrefetcherBase *createASPPrefetcher(String mmu_name, String prefetcher_name, String pq_index, Core *core, MemoryManagerBase *memory_manager, ShmemPerfModel *shmem_perf_model)
		{
			int table_size = Sim()->getCfg()->getInt("perf_model/"+mmu_name+"/tlb_prefetch/pq" + pq_index + "/asp_prefetcher/table_size");
			int prefetch_threshold = Sim()->getCfg()->getInt("perf_model/"+mmu_name+"/tlb_prefetch/pq" + pq_index + "/asp_prefetcher/prefetch_threshold");
			bool extra_prefetch = Sim()->getCfg()->getBool("perf_model/"+mmu_name+"/tlb_prefetch/pq" + pq_index + "/asp_prefetcher/extra_prefetch");
			int lookahead = Sim()->getCfg()->getInt("perf_model/"+mmu_name+"/tlb_prefetch/pq" + pq_index + "/asp_prefetcher/lookahead");
			int degree = Sim()->getCfg()->getInt("perf_model/"+mmu_name+"/tlb_prefetch/pq" + pq_index + "/asp_prefetcher/degree");
			return new ArbitraryStridePrefetcher(core, memory_manager, shmem_perf_model, table_size, prefetch_threshold, extra_prefetch, lookahead, degree, prefetcher_name);
		}
		static TLBPrefetcherBase *createTLBPrefetcherStride(String mmu_name, String prefetcher_name, String pq_index, Core *core, MemoryManagerBase *memory_manager, ShmemPerfModel *shmem_perf_model)
		{
			int length = Sim()->getCfg()->getInt("perf_model/"+mmu_name+"/tlb_prefetch/pq" + pq_index + "/stride_prefetcher/length");
			return new StridePrefetcher(core, memory_manager, shmem_perf_model, length, prefetcher_name);
		}
		static TLBPrefetcherBase *createTLBPrefetcherH2(String mmu_name, String prefetcher_name, String pq_index, Core *core, MemoryManagerBase *memory_manager, ShmemPerfModel *shmem_perf_model)
		{
			return new H2Prefetcher(core, memory_manager, shmem_perf_model, prefetcher_name);
		}
		static TLBPrefetcherBase *createTemporalPTEPrefetcher(String mmu_name, String prefetcher_name, String pq_index, Core *core, MemoryManagerBase *memory_manager, ShmemPerfModel *shmem_perf_model)
		{
			String cfg_base = "perf_model/" + mmu_name + "/tlb_prefetch/pq" + pq_index + "/temporal_pte_prefetcher/";

			uint32_t num_offsets  = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "num_offsets"));
			uint32_t offset_bits  = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "offset_bits"));
			uint32_t conf_bits    = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "conf_bits"));
			uint32_t base_bit     = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "base_bit"));
			uint32_t conf_threshold = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "conf_threshold"));
			uint32_t page_shift   = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "page_shift"));
			uint32_t region_shift = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "region_shift"));
			uint32_t conf_init    = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "conf_init"));
			bool enable_decay     = Sim()->getCfg()->getBool(cfg_base + "enable_decay");
			uint64_t decay_period = static_cast<uint64_t>(Sim()->getCfg()->getInt(cfg_base + "decay_period"));
			uint32_t pc_tag_bits  = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "pc_tag_bits"));
			uint32_t pc_table_size = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "pc_table_size"));
			bool reserve_global_slot = Sim()->getCfg()->getBool(cfg_base + "reserve_global_slot");
			uint32_t max_prefetch_depth = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "max_prefetch_depth"));
			float chain_edge_decay = Sim()->getCfg()->hasKey(cfg_base + "chain_edge_decay")
				? static_cast<float>(Sim()->getCfg()->getFloat(cfg_base + "chain_edge_decay")) : 0.75f;
			float chain_score_threshold = Sim()->getCfg()->hasKey(cfg_base + "chain_score_threshold")
				? static_cast<float>(Sim()->getCfg()->getFloat(cfg_base + "chain_score_threshold")) : 0.20f;
			uint32_t chain_conf_threshold = Sim()->getCfg()->hasKey(cfg_base + "chain_conf_threshold")
				? static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "chain_conf_threshold")) : 0;
			bool learn_on_hit = Sim()->getCfg()->getBool(cfg_base + "learn_on_hit");
			bool prefetch_on_hit = Sim()->getCfg()->getBool(cfg_base + "prefetch_on_hit");
			uint32_t stride_conf_threshold = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "stride_conf_threshold"));
			bool stride_direct_prefetch = Sim()->getCfg()->getBool(cfg_base + "stride_direct_prefetch");
			uint32_t stride_direct_degree = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "stride_direct_degree"));
			bool virtualize_pc_table = Sim()->getCfg()->getBool(cfg_base + "virtualize_pc_table");

			// Confidence policy
			std::string confidence_policy = Sim()->getCfg()->hasKey(cfg_base + "confidence_policy")
				? Sim()->getCfg()->getString(cfg_base + "confidence_policy").c_str() : "competitive";
			uint32_t conf_bump_amount = Sim()->getCfg()->hasKey(cfg_base + "conf_bump_amount")
				? static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "conf_bump_amount")) : 1;
			uint32_t conf_decay_on_bump = Sim()->getCfg()->hasKey(cfg_base + "conf_decay_on_bump")
				? static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "conf_decay_on_bump")) : 1;
			uint32_t conf_decay_on_miss = Sim()->getCfg()->hasKey(cfg_base + "conf_decay_on_miss")
				? static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "conf_decay_on_miss")) : 1;

			// OS-managed side-table payload variant (alternative to PTE-embedded)
			bool payload_in_side_table = Sim()->getCfg()->getBoolDefault(cfg_base + "payload_in_side_table", false);
			uint64_t side_table_base_pa = Sim()->getCfg()->hasKey(cfg_base + "side_table_base_pa")
				? static_cast<uint64_t>(Sim()->getCfg()->getInt(cfg_base + "side_table_base_pa")) : (120ULL << 30);
			uint32_t side_table_payload_bits = Sim()->getCfg()->hasKey(cfg_base + "side_table_payload_bits")
				? static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "side_table_payload_bits")) : 0;  // 0 = auto (codec width)
			bool model_payload_writeback = Sim()->getCfg()->getBoolDefault(cfg_base + "model_payload_writeback", false);
			bool prefetch_install_pq = Sim()->getCfg()->getBoolDefault(cfg_base + "prefetch_install_pq", true);
			bool side_table_radix = Sim()->getCfg()->getBoolDefault(cfg_base + "side_table_radix", true);
			uint32_t side_table_levels = Sim()->getCfg()->hasKey(cfg_base + "side_table_levels")
				? static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "side_table_levels")) : 3;
			uint32_t side_table_bits_per_level = Sim()->getCfg()->hasKey(cfg_base + "side_table_bits_per_level")
				? static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "side_table_bits_per_level")) : 9;
			uint32_t side_table_pwc_entries = Sim()->getCfg()->hasKey(cfg_base + "side_table_pwc_entries")
				? static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "side_table_pwc_entries")) : 32;

			String mode_str = Sim()->getCfg()->getString(cfg_base + "mode");
			TemporalPTEMode mode = TemporalPTEMode::PTE_ONLY;
			if (mode_str == "learn_pte_on_transitions")
				mode = TemporalPTEMode::LEARN_PTE_ON_TRANSITIONS;
			else if (mode_str == "pc_cond_learn")
				mode = TemporalPTEMode::PC_COND_LEARN;

			String repl_str = Sim()->getCfg()->getString(cfg_base + "replacement");
			TemporalPTEReplacement repl = TemporalPTEReplacement::LOWEST_CONF;
			if (repl_str == "random")
				repl = TemporalPTEReplacement::RANDOM;
			else if (repl_str == "lru")
				repl = TemporalPTEReplacement::LRU;

			return new TemporalPTEPrefetcher(core, memory_manager, shmem_perf_model, prefetcher_name,
				num_offsets, offset_bits, conf_bits, base_bit,
				conf_threshold, page_shift, region_shift,
				mode, repl, conf_init, enable_decay, decay_period,
				pc_tag_bits, pc_table_size, reserve_global_slot,
				max_prefetch_depth, chain_edge_decay, chain_score_threshold,
				chain_conf_threshold,
				learn_on_hit, prefetch_on_hit,
				stride_conf_threshold,
				stride_direct_prefetch, stride_direct_degree,
				virtualize_pc_table,
				confidence_policy, conf_bump_amount,
				conf_decay_on_bump, conf_decay_on_miss,
				payload_in_side_table, side_table_base_pa, side_table_payload_bits,
				side_table_radix, side_table_levels, side_table_bits_per_level, side_table_pwc_entries,
				model_payload_writeback, prefetch_install_pq);
		}
		static TLBPrefetcherBase *createBertiTLBPrefetcher(String mmu_name, String prefetcher_name, String pq_index, Core *core, MemoryManagerBase *memory_manager, ShmemPerfModel *shmem_perf_model)
		{
			String cfg_base = "perf_model/" + mmu_name + "/tlb_prefetch/pq" + pq_index + "/berti_prefetcher/";

			uint32_t region_bits          = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "region_bits"));
			uint32_t current_pages        = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "current_pages"));
			uint32_t prev_requests        = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "prev_requests"));
			uint32_t record_pages         = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "record_pages"));
			uint32_t ip_table_entries     = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "ip_table_entries"));
			uint32_t num_berti            = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "num_berti"));
			uint32_t max_burst            = static_cast<uint32_t>(Sim()->getCfg()->getInt(cfg_base + "max_burst"));

			return new BertiTLBPrefetcher(core, memory_manager, shmem_perf_model, prefetcher_name,
				region_bits, current_pages, prev_requests, record_pages,
				ip_table_entries, num_berti, max_burst);
		}
		static TLBPrefetcherBase *createTLBPrefetcher(String mmu_name, String prefetcher_name, String pq_index, Core *core, MemoryManagerBase *memory_manager, ShmemPerfModel *shmem_perf_model)
		{
			if (prefetcher_name == "stride")
			{
				return createTLBPrefetcherStride(mmu_name, prefetcher_name, pq_index, core, memory_manager, shmem_perf_model);
			}
			else if (prefetcher_name == "h2")
			{
				return createTLBPrefetcherH2(mmu_name, prefetcher_name, pq_index, core, memory_manager, shmem_perf_model);
			}
			else if (prefetcher_name == "asp")
			{

				return createASPPrefetcher(mmu_name, prefetcher_name, pq_index, core, memory_manager, shmem_perf_model);
			}
			else if (prefetcher_name == "temporal_pte")
			{
				return createTemporalPTEPrefetcher(mmu_name, prefetcher_name, pq_index, core, memory_manager, shmem_perf_model);
			}
			else if (prefetcher_name == "berti")
			{
				return createBertiTLBPrefetcher(mmu_name, prefetcher_name, pq_index, core, memory_manager, shmem_perf_model);
			}
			else
			{
				std::cout << "[CONFIG ERROR] No such TLB prefetcher: " << prefetcher_name << std::endl;

				return nullptr;
			}
		}
	};
}
