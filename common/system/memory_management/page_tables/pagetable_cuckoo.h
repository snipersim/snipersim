#pragma once
#include "pagetable.h"
#include "pmd_cwt_entry.h"
#include "sim_log.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include <stdint.h>
#include <vector>
#include <unordered_map>

namespace ParametricDramDirectoryMSI
{
	/**
	 * @brief The elastic cuckoo page table [Skarlatos et al., Elastic Cuckoo
	 *        Page Tables, ASPLOS 2020], after the authors' released code.
	 *
	 * One cuckoo hash table per page size, each of `ways` arrays of 64-byte
	 * elements; an element holds a tag (VPN >> 3) and the eight translations
	 * of that aligned block of pages.  A walk hashes the tag into every way
	 * of every page size's table and reads those elements in parallel (depth
	 * 0 for all; table_level = page-size index), so the MMU charges one
	 * round of memory accesses.  An insert that finds its ways taken evicts
	 * the occupant into its other way, up to a retry limit; a table past
	 * rehash_threshold, or an insert that fails, is grown by `scale` and
	 * rehashed.  The PMD-level Cuckoo Walk Table records, per 16 MB section,
	 * which page sizes it holds and in which way, for the Cuckoo Walk Cache
	 * (cuckoo_filter.cc) to cut walks down with.
	 */
	class PageTableCuckoo : public PageTable
	{
	private:
		struct Element
		{
			IntPtr tag;
			bool validityBits[8];
			IntPtr frames[8];

			Element() : tag(static_cast<IntPtr>(-1))
			{
				for (int i = 0; i < 8; ++i)
				{
					validityBits[i] = false;
					frames[i] = static_cast<IntPtr>(-1);
				}
			}
		};

		Element ***tables;        // [page size][way][slot]
		UInt64 **table_ppns;      // Frame number of each way's array

		int *numItems;            // Tags stored per page size
		double loadFactor;        // Rehash above this items / capacity
		int *m_page_table_sizes;
		int m_ways;
		float m_scale;

		std::vector<Element> nonResidentEntries;   // Displaced by a failed insert, until the next rehash

		struct
		{
			UInt64 page_walks_total;
			UInt64 cuckoo_evictions;
			UInt64 rehashes;
			UInt64 page_faults;
			UInt64 prefetch_page_faults;
			UInt64 *cuckoo_accesses;
			UInt64 *cuckoo_hits_per_level;
		} cuckoo_stats;

		SimLog *m_log;

		std::unordered_map<IntPtr, PmdCwtEntry> pmd_cwt_entries;   // The PMD-CWT, by 1 GB tag

		int pageSizeIndex(int page_size) const;

	public:
		PageTableCuckoo(int core_id, String name, String type, int page_sizes, int *page_size_list,
		                int *page_table_sizes, double rehash_threshold, float scale, int ways, bool is_guest = false);
		~PageTableCuckoo();

		PTWResult initializeWalk(IntPtr address, bool count, bool is_prefetch = false, bool restart_walk = false);
		int updatePageTableFrames(IntPtr address, IntPtr core_id, IntPtr ppn, int page_size, std::vector<UInt64> frames);
		void deletePage(IntPtr address);
		String getType() { return "elastic_cuckoo"; };

		UInt64 hash(UInt64 key, int table_size);
		bool insertElement(int page_size_index, const Element &entry, IntPtr address);
		bool rehash(int page_size_index, int new_size);
		double currentLoadFactor(int page_size_index) const;
		IntPtr getPhysicalSpace(int size);

		// The PMD-CWT row of an address, created empty if absent.  The row
		// stays where it is for the life of the table, so the Cuckoo Walk
		// Cache keeps the pointer.
		PmdCwtEntry *retrieveCWTrow(IntPtr address);
	};
}
