#ifndef PWC_H
#define PWC_H

#include "fixed_types.h"
#include "cache.h"
#include <util.h>
#include <unordered_map>
#include "lock.h"
#include <vector>

namespace ParametricDramDirectoryMSI
{
	class PWC
	{
	private:
		static UInt32 SIM_PAGE_SHIFT; // 4KB
		static IntPtr SIM_PAGE_SIZE;
		static IntPtr SIM_PAGE_MASK;

		Cache **m_cache; // @kanellok Cache PMLE4

		UInt64 *m_access, *m_miss;
		int num_caches;
		core_id_t m_core_id;
		bool perfect;

	public:
		enum where_t
		{
			HIT = 0,
			MISS
		};

		ComponentLatency access_latency;
		ComponentLatency miss_latency;

		PWC(String name, String cfgname, core_id_t core_id, UInt32 *associativities, UInt32 *entries, int num_caches, ComponentLatency _access_latency, ComponentLatency _miss_latency, bool _perfect);
		bool lookup(IntPtr address, SubsecondTime now, bool allocate_on_miss, int level, bool count, IntPtr ppn = 0);
		void allocate(IntPtr address, SubsecondTime now, int cache_index, IntPtr ppn);
		static const UInt64 HASH_PRIME = 124183;
	};
}

#endif // TLB_H
