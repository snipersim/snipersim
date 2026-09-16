
#include "tlb_prefetcher_base.h"
#include "stride_prefetcher.h"
#include "cache_cntlr.h"
#include "stats.h"
// #define DEBUG

namespace ParametricDramDirectoryMSI
{
	StridePrefetcher::StridePrefetcher(Core *_core, MemoryManagerBase *_memory_manager, ShmemPerfModel *_shmem_perf_model, int length, String name) :
	TLBPrefetcherBase(_core, _memory_manager, _shmem_perf_model, name), core(_core),
		memory_manager(_memory_manager),
		shmem_perf_model(_shmem_perf_model), length(length)
	{
		stats.prefetch_attempts = 0;
		stats.successful_prefetches = 0;
		stats.failed_prefetches = 0;

		std::cout << "Stride prefetcher created with length " << length << std::endl;
		registerStatsMetric("tlb_stride", core->getId(), "prefetch_attempts", &stats.prefetch_attempts);
		registerStatsMetric("tlb_stride", core->getId(), "successful_prefetches", &stats.successful_prefetches);
		registerStatsMetric("tlb_stride", core->getId(), "failed_prefetches", &stats.failed_prefetches);

	}
	std::vector<query_entry> StridePrefetcher::performPrefetch(IntPtr address, IntPtr eip, Core::lock_signal_t lock, bool modeled, bool count, PageTable *pt, bool instruction, bool tlb_hit, bool pq_hit, int /*page_size*/)
	{
		vector<query_entry> result;
		IntPtr VPN = address >> 12; // We assume that the page size is 4KB
		for (int i = -length; i <= length; i++)
		{
			if (i != 0)
			{
				stats.prefetch_attempts++;
				query_entry result_ptw = PTWTransparent((VPN + i) << 12, eip, lock, modeled, count, pt);
				#ifdef DEBUG
					std::cout << "Stride Prefetcher: VPN: " << VPN+i << " i: " << i << " PPN: " << result_ptw.ppn << std::endl;
				#endif
				if(result_ptw.ppn!=0){
					stats.successful_prefetches++;
					result.push_back(result_ptw);
				}
				else{
					stats.failed_prefetches++;
				}

			}
		}
		return result;
	}

}
