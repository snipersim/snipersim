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
	 * @brief A hashed page table with chaining.
	 *
	 * One table per page size.  A slot holds a tag (VPN >> 3) and the eight
	 * translations of that aligned block of eight pages, so one 64-byte line
	 * resolves any of them; a slot whose tag differs chains to another entry
	 * carved from the kernel region.  A walk probes every page size's table
	 * (table_level = page-size index) and follows the chain (depth = chain
	 * step), which is how the MMU charges it: the tables in parallel, the
	 * chain in sequence.  A table that fills past resize_threshold is grown
	 * by resize_factor and rehashed.
	 */
	class PageTableHT : public PageTable
	{

	private:
		struct Entry
		{
			IntPtr tag;
			IntPtr ppn[8];
			bool valid[8];
			Entry *next_entry;
			IntPtr emulated_physical_address;   // Byte address of this 64-byte entry
		};

		std::vector<Entry *> page_tables;
		IntPtr *table_pa;                       // Byte address of each table's root array
		int *m_page_table_sizes;

		double m_resize_threshold;              // Occupancy ratio that triggers a resize
		int    m_resize_factor;                 // Growth factor on resize
		int   *m_entry_count;                   // Occupied root slots per page size

		struct Stats
		{
			UInt64 *page_table_walks;
			UInt64 *chained;
			UInt64 *num_accesses;
			UInt64 *resizes;
			UInt64 page_faults;
			UInt64 prefetch_page_faults;
		} stats;

		SimLog *m_log;

		IntPtr tableBytes(int size);
		void resizeTable(int page_size_index);
		int pageSizeIndex(int page_size) const;

	public:
		PageTableHT(int core_id, String name, String type, int page_sizes, int *page_size_list, int *page_table_sizes, bool is_guest = false);
		~PageTableHT();

		UInt64 hashFunction(IntPtr address, int table_size);

		PTWResult initializeWalk(IntPtr address, bool count, bool is_prefetch = false, bool restart_walk = false);
		int updatePageTableFrames(IntPtr address, IntPtr core_id, IntPtr ppn, int page_size, std::vector<UInt64> frames);
		void deletePage(IntPtr address);
		String getType() { return "hash_table"; };
	};
}
