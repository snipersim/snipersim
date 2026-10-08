// ============================================================================
// Utopia MMU with the coalesced 2MB radix walk (see mmu_utopia_coalesce.h)
// ============================================================================

#include "mmu_utopia_coalesce.h"
#include "memory_management/physical_memory_allocators/utopia.h"
#include "core.h"
#include "thread.h"
#include "stats.h"
#include <iostream>

using namespace std;

namespace ParametricDramDirectoryMSI
{

	MemoryManagementUnitUtopiaCoalesce::MemoryManagementUnitUtopiaCoalesce(Core *_core, MemoryManagerBase *_memory_manager, ShmemPerfModel *_shmem_perf_model, String _name, MemoryManagementUnitBase *_nested_mmu)
		: MemoryManagementUnitUtopia(_core, _memory_manager, _shmem_perf_model, _name, _nested_mmu)
	{
		coalesce_stats = CoalesceStats();
		registerStatsMetric(name, core->getId(), "radix_2mb_lookups", &coalesce_stats.radix_2mb_lookups);
		registerStatsMetric(name, core->getId(), "radix_2mb_hits", &coalesce_stats.radix_2mb_hits);
		registerStatsMetric(name, core->getId(), "radix_2mb_misses", &coalesce_stats.radix_2mb_misses);
		registerStatsMetric(name, core->getId(), "radix_2mb_latency_total", &coalesce_stats.radix_2mb_latency_total);
		registerRadixWalkCounters("radix_2mb", coalesce_stats.radix_2mb);
		if (core->getId() == 0)
			std::cout << "[MMU::UtopiaCoalesce] RestSeg walk: the coalesced 2MB radix table first, the 4KB one on a miss" << std::endl;
	}

	MemoryManagementUnitUtopiaCoalesce::~MemoryManagementUnitUtopiaCoalesce()
	{
	}

	std::tuple<int, IntPtr, SubsecondTime> MemoryManagementUnitUtopiaCoalesce::RestSegWalk(IntPtr address, bool instruction, IntPtr eip, Core::lock_signal_t lock, bool modeled, bool count)
	{
		// Only the radix walk knows the coalesced table
		if (!radix_lookup_enabled || num_restsegs == 0)
			return MemoryManagementUnitUtopia::RestSegWalk(address, instruction, eip, lock, modeled, count);

		SubsecondTime t_start = shmem_perf_model->getElapsedTime(ShmemPerfModel::_USER_THREAD);
		int app_id = core->getThread() ? core->getThread()->getAppId() : 0;

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
			int page_size_bits = 0;
			IntPtr physical_address = 0;
		};
		std::vector<RestSegAccess> rs_access(num_restsegs);

		// A 4KB RestSeg: the 2MB table, then on a miss the per-page table; a
		// 2MB RestSeg: the per-page table only
		auto accessRestSeg = [&](int i, SubsecondTime access_start) -> SubsecondTime {
			RestSeg *restseg = m_utopia->getRestSeg(i);
			RestSegStats &s = rs_stats[i];
			rs_access[i].page_size_bits = restseg->getPageSizeBits();
			SubsecondTime total_latency = SubsecondTime::Zero();
			SubsecondTime current_time = access_start;

			if (restseg->getPageSizeBits() == 12)
			{
				if (count) coalesce_stats.radix_2mb_lookups++;
				UInt64 aligned_vpn = (address >> 12) & ~(UInt64)511;
				int way_2mb = restseg->lookupCoalesced2MBWay(aligned_vpn, app_id);
				std::vector<std::pair<IntPtr, bool>> levels_2mb;
				restseg->calculateRadixLevelAddressesCoalesced(address, app_id, levels_2mb);
				SubsecondTime lat_2mb = chargeRadixWalk(levels_2mb, current_time, eip, lock, modeled, count, instruction, coalesce_stats.radix_2mb);
				total_latency += lat_2mb;
				current_time = current_time + lat_2mb;
				if (count) coalesce_stats.radix_2mb_latency_total += lat_2mb;
				if (way_2mb >= 0)
				{
					if (count)
					{
						coalesce_stats.radix_2mb_hits++;
						s.radix_lookups++;
						s.radix_hits++;
					}
					rs_access[i].hit = true;
					rs_access[i].page_size_bits = 21;
					rs_access[i].physical_address = restseg->coalescedRegionPPN(address & ~((IntPtr)(2 << 20) - 1), way_2mb);
					restseg->touchWayLRU(address, way_2mb);
					return total_latency;
				}
				if (count) coalesce_stats.radix_2mb_misses++;
			}

			int way = -1;
			bool radix_hit = restseg->lookupWayRadix(address, app_id, way);
			if (radix_hit && way >= 0)
			{
				rs_access[i].hit = true;
				rs_access[i].physical_address = restseg->calculatePPNFromWay(address, way);
				restseg->touchWayLRU(address, way);
			}
			if (count)
			{
				s.radix_lookups++;
				if (radix_hit) s.radix_hits++; else s.radix_misses++;
			}
			std::vector<std::pair<IntPtr, bool>> levels;
			restseg->calculateRadixLevelAddresses(address, app_id, levels);
			SubsecondTime lat = chargeRadixWalk(levels, current_time, eip, lock, modeled, count, instruction, translation_stats.radix);
			total_latency += lat;
			if (count) s.radix_latency += total_latency;
			return total_latency;
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
			SubsecondTime so_far = accessRestSeg(first_restseg, t_start);
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
			if (hit_restseg >= 0)
				result_latency = rs_lat[hit_restseg];
			else
				for (int i = 0; i < num_restsegs; i++)
					result_latency = max(result_latency, rs_lat[i]);
		}

		int page_size = -1;
		IntPtr final_address = 0;
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

} // namespace ParametricDramDirectoryMSI
