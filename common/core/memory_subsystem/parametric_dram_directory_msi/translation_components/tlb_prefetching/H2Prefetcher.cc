
#include "tlb_prefetcher_base.h"
#include "H2Prefetcher.h"
#include "cache_cntlr.h"
#include "stats.h"
namespace ParametricDramDirectoryMSI
{
	H2Prefetcher::H2Prefetcher(Core *_core, MemoryManagerBase *_memory_manager, ShmemPerfModel *_shmem_perf_model, String name) :
		TLBPrefetcherBase(_core, _memory_manager, _shmem_perf_model, name), core(_core),
			memory_manager(_memory_manager),
			shmem_perf_model(_shmem_perf_model)
	{
		A_address = 0;
		B_address = 0;
		C_address = 0;
		stats.prefetch_attempts = stats.successful_prefetches = stats.failed_prefetches = 0;
		registerStatsMetric("tlb_h2", core->getId(), "prefetch_attempts", &stats.prefetch_attempts);
		registerStatsMetric("tlb_h2", core->getId(), "successful_prefetches", &stats.successful_prefetches);
		registerStatsMetric("tlb_h2", core->getId(), "failed_prefetches", &stats.failed_prefetches);
	}
	std::vector<query_entry> H2Prefetcher::performPrefetch(IntPtr address, IntPtr eip, Core::lock_signal_t lock, bool modeled, bool count, PageTable *pt, bool instruction, bool tlb_hit, bool pq_hit, int /*page_size*/)
	{
		vector<query_entry> result;
		A_address = B_address;
		B_address = C_address;
		C_address = address >> 12;
		IntPtr VPN = address >> 12; // We assume that the page size is 4KB

		if (A_address != 0 && B_address != 0 && C_address != 0)
		{
			// The two candidates: the last stride and the one before it
			IntPtr candidates[2] = { VPN + (C_address - B_address), VPN + (B_address - A_address) };
			for (int i = 0; i < 2; i++)
			{
				stats.prefetch_attempts++;
				query_entry q = PTWTransparent(candidates[i] << 12, eip, lock, modeled, count, pt);
				if (q.ppn != 0)
				{
					stats.successful_prefetches++;
					result.push_back(q);
				}
				else
					stats.failed_prefetches++;
			}
		}
		// std::cout << result.size() << std::endl;
		return result;
	}

}
