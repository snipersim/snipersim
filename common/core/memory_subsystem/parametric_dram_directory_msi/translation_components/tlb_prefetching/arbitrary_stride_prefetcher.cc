#include "arbitrary_stride_prefetcher.h"
#include "cache_cntlr.h"
#include "stats.h"
#include <cstring>
#include <iostream>

namespace ParametricDramDirectoryMSI
{

	ArbitraryStridePrefetcher::ArbitraryStridePrefetcher(
		Core *_core, MemoryManagerBase *_memory_manager, ShmemPerfModel *_shmem_perf_model,
		int table_bits, int _prefetch_threshold, bool _extra_prefetch,
		int _lookahead, int _degree, String name, bool _install_pq)
		: TLBPrefetcherBase(_core, _memory_manager, _shmem_perf_model, name),
		  core(_core),
		  memory_manager(_memory_manager),
		  shmem_perf_model(_shmem_perf_model),
		  prefetch_threshold(_prefetch_threshold),
		  extra_prefetch(_extra_prefetch),
		  lookahead(_lookahead),
		  degree(_degree),
		  install_pq(_install_pq)
	{
		int entries = 1 << table_bits;
		table_size = entries;
		table = new entry_prefetcher[entries];

		for (int i = 0; i < entries; i++)
		{
			table[i].PC = 0;
			table[i].vaddr = 0;
			table[i].stride = -1;
			table[i].saturation_counter = 0;
		}

		memset(&stats, 0, sizeof(stats));

		log_file_name = std::string(name.c_str()) + ".log." + std::to_string(core->getId());
		log_file_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + log_file_name;
		log_file.open(log_file_name);

		std::cout << "Arbitrary Stride prefetcher created with "
				  << "table bits: " << table_bits << " prefetch threshold: "
				  << _prefetch_threshold << std::endl;

		registerStatsMetric("asp_tlb", core->getId(), "successful_prefetches", &stats.successful_prefetches);
		registerStatsMetric("asp_tlb", core->getId(), "prefetch_attempts", &stats.prefetch_attempts);
		registerStatsMetric("asp_tlb", core->getId(), "failed_prefetches", &stats.failed_prefetches);

		registerStatsMetric("asp_tlb", core->getId(), "queries", &stats.queries);
		registerStatsMetric("asp_tlb", core->getId(), "pc_hits", &stats.pc_hits);
		registerStatsMetric("asp_tlb", core->getId(), "pc_misses", &stats.pc_misses);
		registerStatsMetric("asp_tlb", core->getId(), "pc_evictions", &stats.pc_evictions);

		registerStatsMetric("asp_tlb", core->getId(), "new_stride_observed", &stats.new_stride_observed);
		registerStatsMetric("asp_tlb", core->getId(), "stride_same", &stats.stride_same);
		registerStatsMetric("asp_tlb", core->getId(), "stride_change", &stats.stride_change);
		registerStatsMetric("asp_tlb", core->getId(), "zero_stride", &stats.zero_stride);
		registerStatsMetric("asp_tlb", core->getId(), "positive_stride", &stats.positive_stride);
		registerStatsMetric("asp_tlb", core->getId(), "negative_stride", &stats.negative_stride);

		registerStatsMetric("asp_tlb", core->getId(), "threshold_reached", &stats.threshold_reached);
		registerStatsMetric("asp_tlb", core->getId(), "trained_entries", &stats.trained_entries);
		registerStatsMetric("asp_tlb", core->getId(), "trained_but_no_prefetch", &stats.trained_but_no_prefetch);

		registerStatsMetric("asp_tlb", core->getId(), "extra_prefetches_issued", &stats.extra_prefetches_issued);
		registerStatsMetric("asp_tlb", core->getId(), "extra_prefetches_successful", &stats.extra_prefetches_successful);
		registerStatsMetric("asp_tlb", core->getId(), "extra_prefetches_failed", &stats.extra_prefetches_failed);

		registerStatsMetric("asp_tlb", core->getId(), "sum_abs_stride", &stats.sum_abs_stride);
		registerStatsMetric("asp_tlb", core->getId(), "prefetch_distance_sum", &stats.prefetch_distance_sum);
		registerStatsMetric("asp_tlb", core->getId(), "max_saturation_counter", &stats.max_saturation_counter);
		registerStatsMetric("asp_tlb", core->getId(), "table_accesses", &stats.table_accesses);
	}

	std::vector<query_entry> ArbitraryStridePrefetcher::performPrefetch(
		IntPtr address, IntPtr eip, Core::lock_signal_t lock, bool modeled, bool count, PageTable *pt, bool instruction, bool tlb_hit, bool pq_hit, int /*page_size*/)
	{
		std::vector<query_entry> result;
		int index = eip % table_size;
		if (index < 0)
			index = -index;

		IntPtr VPN = address >> 12;

		stats.queries++;
		stats.table_accesses++;

		if (table[index].PC == eip)
		{
			stats.pc_hits++;

			long long new_stride = static_cast<long long>(VPN) - static_cast<long long>(table[index].vaddr);

			if (new_stride == 0)
				stats.zero_stride++;
			else if (new_stride > 0)
				stats.positive_stride++;
			else
				stats.negative_stride++;

			if (table[index].stride == -1)
			{
				table[index].stride = new_stride;
				table[index].saturation_counter++;
				stats.new_stride_observed++;
			}
			else if (table[index].stride == new_stride)
			{
				table[index].saturation_counter++;
				stats.stride_same++;
			}
			else
			{
				stats.stride_change++;
				table[index].saturation_counter = 0;
				table[index].stride = new_stride;
			}

			if (table[index].saturation_counter > stats.max_saturation_counter)
				stats.max_saturation_counter = table[index].saturation_counter;

			if (table[index].saturation_counter > static_cast<unsigned int>(prefetch_threshold))
			{
				stats.threshold_reached++;

				long long abs_stride = (table[index].stride >= 0) ? table[index].stride : -table[index].stride;
				stats.sum_abs_stride += static_cast<UInt64>(abs_stride);

				// Agile TLB Prefetching's ASP: degree pages at a time, starting
				// lookahead strides ahead -- VPN + (lookahead + d) * stride for
				// d in [0, degree).  (This used to take one step, VPN + stride,
				// whatever the two knobs said.)
				for (int d = 0; d < degree; d++)
				{
					long long offset = static_cast<long long>(lookahead + d) * table[index].stride;
					IntPtr prefetch_vpn = static_cast<IntPtr>(static_cast<long long>(VPN) + offset);
					stats.prefetch_distance_sum += static_cast<UInt64>(offset >= 0 ? offset : -offset);

					query_entry prefetch_result = PTWTransparent(prefetch_vpn << 12, eip, lock, modeled, count, pt);
					stats.prefetch_attempts++;

					if (prefetch_result.ppn != 0)
					{
						stats.successful_prefetches++;
						stats.trained_entries++;
						result.push_back(prefetch_result);
					}
					else
					{
						stats.failed_prefetches++;
						stats.trained_but_no_prefetch++;
					}
				}

				if (extra_prefetch)
				{
					stats.extra_prefetches_issued++;
					long long extra_vpn_signed = static_cast<long long>(VPN) - table[index].stride;
					IntPtr extra_vpn = static_cast<IntPtr>(extra_vpn_signed);
					query_entry extra_result = PTWTransparent(extra_vpn << 12, eip, lock, modeled, count, pt);

					if (extra_result.ppn != 0)
					{
						stats.extra_prefetches_successful++;
						stats.successful_prefetches++;
						result.push_back(extra_result);
					}
					else
					{
						stats.extra_prefetches_failed++;
						stats.failed_prefetches++;
					}
				}
			}

			table[index].vaddr = VPN;
		}
		else
		{
			stats.pc_misses++;
			if (table[index].PC != 0)
				stats.pc_evictions++;

			table[index].PC = eip;
			table[index].vaddr = VPN;
			table[index].stride = -1;
			table[index].saturation_counter = 0;
		}

		// Cache-only mode: the walks above have brought the PTE lines into the
		// cache hierarchy; dropping their results installs nothing in the PQ.
		if (!install_pq)
			result.clear();

		return result;
	}

}
