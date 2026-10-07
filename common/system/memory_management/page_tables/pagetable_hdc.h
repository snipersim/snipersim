#pragma once
#include "pagetable.h"
#include "sim_log.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include <stdint.h>
#include <vector>

namespace ParametricDramDirectoryMSI
{
	/**
	 * @brief The hashed page table of Hash, Don't Cache [Yaniv and Tsafrir,
	 *        SIGMETRICS 2016]: open addressing with linear probing.
	 *
	 * One table per page size, each slot a 64-byte line holding a tag
	 * (VPN >> 3) and the eight translations of that aligned block of pages.
	 * A collision probes the next slot, so a walk is a sequence of line
	 * reads (depth = probe step) in every page size's table (table_level =
	 * page-size index).  A table that fills past resize_threshold is grown
	 * by resize_factor and rehashed.
	 */
	class PageTableHDC : public PageTable
	{

	private:
		struct Entry
		{
			bool valid[8];
			IntPtr tag;
			IntPtr ppn[8];
			int distance_from_root;   // Probes from the home slot, for the hash's quality
		};
		std::vector<Entry *> page_tables;

		int *m_page_table_sizes;
		IntPtr *emulated_table_address;   // Byte address of each table

		double m_resize_threshold;
		int    m_resize_factor;
		int   *m_entry_count;

		struct Stats
		{
			UInt64 *page_table_walks;
			UInt64 *num_accesses;
			UInt64 *collisions;
			UInt64 *resizes;
			UInt64 page_faults;
			UInt64 prefetch_page_faults;
		} stats;

		SimLog *m_log;

		IntPtr tableBytes(int size);
		int pageSizeIndex(int page_size) const;
		void resizeTable(int page_size_index);

	public:
		PageTableHDC(int core_id, String name, String type, int page_sizes, int *page_size_list, int *page_table_sizes, bool is_guest = false);
		~PageTableHDC();

		UInt64 hashFunction(IntPtr address, int table_size);

		PTWResult initializeWalk(IntPtr address, bool count, bool is_prefetch = false, bool restart_walk = false);
		int updatePageTableFrames(IntPtr address, IntPtr core_id, IntPtr ppn, int page_size, std::vector<UInt64> frames);
		void deletePage(IntPtr address);
		String getType() { return "hash_dont_cache"; };
	};
}
