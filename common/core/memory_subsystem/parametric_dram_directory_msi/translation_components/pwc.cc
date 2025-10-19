#include "pwc.h"
#include "stats.h"
#include "config.hpp"
#include <cmath>
#include <iostream>
#include <utility>
#include "core_manager.h"
#include "cache_set.h"
// #define DEBUG
namespace ParametricDramDirectoryMSI
{

	UInt32 PWC::SIM_PAGE_SHIFT;
	IntPtr PWC::SIM_PAGE_SIZE;
	IntPtr PWC::SIM_PAGE_MASK;

	PWC::PWC(String name, String cfgname, core_id_t core_id, UInt32 *associativities, UInt32 *entries, int _num_caches, ComponentLatency _access_latency, ComponentLatency _miss_latency, bool _perfect)
		: m_core_id(core_id), access_latency(_access_latency), miss_latency(_miss_latency) // Assuming 8B granularity
		  ,
		  num_caches(_num_caches),
		  perfect(_perfect)
	{
		int page_sizes[1] = {0};
		m_cache = (Cache **)malloc(sizeof(Cache *) * num_caches);
		m_access = (UInt64 *)malloc(sizeof(UInt64) * num_caches);
		m_miss = (UInt64 *)malloc(sizeof(UInt64) * num_caches);
		for (int i = 0; i < num_caches; i++)
		{

			m_cache[i] = new Cache(name + "_L" + itostr((num_caches + 1) - i), cfgname, core_id, entries[i] / associativities[i], associativities[i], 8, "lru", CacheBase::PR_L1_CACHE, CacheBase::hash_t::HASH_MASK, NULL, NULL, true, page_sizes, 1);
			registerStatsMetric(name + "_L" + itostr((num_caches + 1) - i), core_id, "access", &m_access[i]);
			registerStatsMetric(name + "_L" + itostr((num_caches + 1) - i), core_id, "miss", &m_miss[i]);
		}
	}

	bool PWC::lookup(IntPtr address, SubsecondTime now, bool allocate_on_miss, int level_index, bool count, IntPtr ppn)
	{
		bool hit;
		
		hit = m_cache[level_index]->accessSingleLine(address, Cache::LOAD, NULL, 0, now, true);
#ifdef DEBUG
		std::cout << "Accessing address: " << address << " at level: " << level_index << " hit: " << hit << std::endl;
#endif
		if (count)
			m_access[level_index]++;
		if (hit)
			return hit;
		else
		{

			if (count)
				m_miss[level_index]++;
			if (allocate_on_miss)
				allocate(address, now, level_index, ppn);
			return hit;
		}
	}

	void PWC::allocate(IntPtr address, SubsecondTime now, int cache_index, IntPtr ppn)
	{
		bool eviction;
		IntPtr evict_addr;
		CacheBlockInfo evict_block_info;

		IntPtr tag;
		UInt32 set_index;
		m_cache[cache_index]->splitAddress(address, tag, set_index);
		m_cache[cache_index]->insertSingleLine(address, NULL, &eviction, &evict_addr, &evict_block_info, NULL, now, NULL, CacheBlockInfo::block_type_t::NON_PAGE_TABLE);
	}

}
