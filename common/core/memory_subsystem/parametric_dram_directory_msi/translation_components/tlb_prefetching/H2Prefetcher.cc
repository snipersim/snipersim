
#include "tlb_prefetcher_base.h"
#include "H2Prefetcher.h"
#include "cache_cntlr.h"
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
			result.push_back(PTWTransparent((VPN + (C_address - B_address)) << 12, eip, lock, modeled, count, pt));
			result.push_back(PTWTransparent((VPN + (B_address - A_address)) << 12, eip, lock, modeled, count, pt));
		}
		// std::cout << result.size() << std::endl;
		return result;
	}

}
