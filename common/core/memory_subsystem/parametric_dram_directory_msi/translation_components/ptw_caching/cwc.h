#pragma once

#include "fixed_types.h"
#include "core.h"
#include "pmd_cwt_entry.h"
#include <list>
#include <unordered_map>

namespace ParametricDramDirectoryMSI
{
	/**
	 * @brief A Cuckoo Walk Cache row: the cached PMD-CWT entry.
	 *
	 * The row points at the page table's own entry, which lives in the
	 * table's map for the life of the table (an unordered_map never moves
	 * its values), so what the cache serves is always the entry's current
	 * contents, and an update to the table is seen by the cache.
	 */
	struct CWCRow
	{
		PmdCwtEntry *cwt_entry_ptr;

		CWCRow(PmdCwtEntry *entry = nullptr) : cwt_entry_ptr(entry) {}
	};

	/**
	 * @brief The Cuckoo Walk Cache of the PMD-CWT [Skarlatos et al., ASPLOS
	 *        2020]: fully associative, LRU, one row per 1 GB tag.
	 */
	class CWCache
	{
	private:
		struct
		{
			UInt64 cwc_hits;
			UInt64 cwc_accesses;
		} cwc_stats;

		String name;
		Core *core;
		int size;
		std::list<CWCRow> m_lru_list;
		std::unordered_map<IntPtr, std::list<CWCRow>::iterator> m_map;

	public:
		CWCache(String name, Core *core, int size);

		// Looks for the row of a tag: true on a hit, with the row in entry.
		bool lookup(IntPtr tag, CWCRow &entry);
		// Inserts a row, evicting the least recently used one when full.
		void insert(const CWCRow &entry);
	};
}
