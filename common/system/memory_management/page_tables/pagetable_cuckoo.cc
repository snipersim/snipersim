#include "pagetable_cuckoo.h"
#include "pagetable_fault.h"
#include "simulator.h"
#include "config.hpp"
#include "city.h"
#include "memory_management/physical_memory_allocators/physical_memory_allocator.h"
#include "mimicos.h"
#include "debug_config.h"

#include <cstdlib>
#include <cassert>
#include <numeric>
#include <algorithm>
#include <random>

namespace ParametricDramDirectoryMSI
{
	static MimicOS *osForPageTable(bool is_guest)
	{
		return is_guest ? Sim()->getMimicOS_VM() : Sim()->getMimicOS();
	}

	int PageTableCuckoo::pageSizeIndex(int page_size) const
	{
		for (int i = 0; i < m_page_sizes; i++)
			if (m_page_size_list[i] == page_size)
				return i;
		return 0;
	}

	PageTableCuckoo::PageTableCuckoo(int core_id, String name, String type, int page_sizes, int *page_size_list,
	                                 int *page_table_sizes, double rehash_threshold, float scale, int ways, bool is_guest)
		: PageTable(core_id, name, type, page_sizes, page_size_list, is_guest),
		  m_page_table_sizes(page_table_sizes)
	{
		m_log = new SimLog(std::string(name.c_str()) + "_cuckoo", core_id, DEBUG_PAGE_TABLE_HASH);

		numItems = new int[m_page_sizes]();
		loadFactor = rehash_threshold;
		m_scale = scale;
		m_ways = ways;

		tables = new Element **[m_page_sizes];
		table_ppns = new UInt64 *[m_page_sizes];

		for (int a = 0; a < m_page_sizes; a++)
		{
			tables[a] = new Element *[m_ways];
			table_ppns[a] = new UInt64[m_ways];

			m_log->log("Page size ", m_page_size_list[a], ": ", m_ways, " ways of ", m_page_table_sizes[a], " elements");

			for (int i = 0; i < m_ways; i++)
			{
				tables[a][i] = new Element[m_page_table_sizes[a]];
				table_ppns[a][i] = getPhysicalSpace(m_page_table_sizes[a] * 64);
				m_log->log("Way ", i, " at frame ", table_ppns[a][i]);
			}
		}

		cuckoo_stats.page_walks_total = 0;
		cuckoo_stats.cuckoo_evictions = 0;
		cuckoo_stats.rehashes = 0;
		cuckoo_stats.page_faults = 0;
		cuckoo_stats.prefetch_page_faults = 0;
		registerStatsMetric(name, core_id, "ptws_total", &cuckoo_stats.page_walks_total);
		registerStatsMetric(name, core_id, "evictions", &cuckoo_stats.cuckoo_evictions);
		registerStatsMetric(name, core_id, "rehashes", &cuckoo_stats.rehashes);
		registerStatsMetric(name, core_id, "page_faults", &cuckoo_stats.page_faults);
		registerStatsMetric(name, core_id, "prefetch_page_faults", &cuckoo_stats.prefetch_page_faults);

		cuckoo_stats.cuckoo_hits_per_level = new UInt64[m_page_sizes]();
		cuckoo_stats.cuckoo_accesses = new UInt64[m_page_sizes]();
		for (int i = 0; i < m_page_sizes; i++)
		{
			registerStatsMetric(name, core_id, "hit_at_level" + itostr(i), &cuckoo_stats.cuckoo_hits_per_level[i]);
			registerStatsMetric(name, core_id, "num_accesses" + itostr(i), &cuckoo_stats.cuckoo_accesses[i]);
		}
	}

	PageTableCuckoo::~PageTableCuckoo()
	{
		for (int i = 0; i < m_page_sizes; ++i)
		{
			for (int j = 0; j < m_ways; ++j)
				delete[] tables[i][j];
			delete[] tables[i];
			delete[] table_ppns[i];
		}
		delete[] tables;
		delete[] table_ppns;
		delete[] numItems;
		delete[] cuckoo_stats.cuckoo_hits_per_level;
		delete[] cuckoo_stats.cuckoo_accesses;
		delete m_log;
	}

	UInt64 PageTableCuckoo::hash(UInt64 address, int table_size)
	{
		return CityHash64((const char *)&address, 8) % table_size;
	}

	IntPtr PageTableCuckoo::getPhysicalSpace(int size)
	{
		return osForPageTable(is_guest)->getMemoryAllocator()->handle_page_table_allocations(size);
	}

	/*
	 * Insert or extend the element of entry's tag for the page at address.
	 * A way already holding the tag takes the page.  Otherwise the cuckoo
	 * algorithm: place the element in a way, evicting the occupant, and
	 * place the occupant in its next way, until a slot is free or the retry
	 * limit is hit -- the last displaced element then waits in
	 * nonResidentEntries for a rehash, and the insert reports failure.
	 */
	bool PageTableCuckoo::insertElement(int page_size_index, const Element &entry, IntPtr address)
	{
		uint64_t VPN = address >> m_page_size_list[page_size_index];
		uint64_t tag = VPN >> 3;
		uint64_t indexInsideBlock = VPN % 8;

		Element currentElement;
		currentElement.tag = entry.tag;
		currentElement.validityBits[indexInsideBlock] = entry.validityBits[indexInsideBlock];
		currentElement.frames[indexInsideBlock] = entry.frames[indexInsideBlock];

		m_log->detailed("Inserting tag ", currentElement.tag, " index ", indexInsideBlock, " in page size ", m_page_size_list[page_size_index]);

		// The tag's element, if a way holds it
		for (int i = 0; i < m_ways; ++i)
		{
			uint64_t pos = hash(tag + i, m_page_table_sizes[page_size_index]);
			Element &slot = tables[page_size_index][i][pos];
			if (slot.tag == tag)
			{
				if (slot.validityBits[indexInsideBlock])
					m_log->log("Page ", SimLog::hex(address), " is already mapped in way ", i);
				slot.frames[indexInsideBlock] = currentElement.frames[indexInsideBlock];
				slot.validityBits[indexInsideBlock] = true;
				return true;
			}
		}

		// A fixed permutation of the ways, cycled for as many retries as allowed
		const int max_retries = 48;
		std::vector<int> permutation(m_ways);
		std::iota(permutation.begin(), permutation.end(), 0);
		std::shuffle(permutation.begin(), permutation.end(), std::default_random_engine(463576468));
		for (int i = m_ways; i < max_retries + 1; ++i)
			permutation.push_back(permutation[(i - m_ways) % m_ways]);

		size_t next = 0;
		int selected_way = permutation[next++];
		Element oldElement = currentElement;

		for (int retries = max_retries; retries > 0; retries--)
		{
			currentElement = oldElement;
			uint64_t pos = hash(currentElement.tag + selected_way, m_page_table_sizes[page_size_index]);
			Element &slot = tables[page_size_index][selected_way][pos];

			if (slot.tag == static_cast<IntPtr>(-1))
			{
				slot = currentElement;
				numItems[page_size_index]++;
				return true;
			}
			else if (slot.tag != currentElement.tag)
			{
				// Evict the occupant to its next way
				m_log->detailed("Evicting tag ", slot.tag, " from way ", selected_way, " slot ", pos);
				oldElement = slot;
				slot = currentElement;
				cuckoo_stats.cuckoo_evictions++;
				selected_way = permutation[next++];
			}
			else
			{
				m_log->log("Tag ", currentElement.tag, " met itself in way ", selected_way);
				return true;
			}
		}

		// Out of retries: the displaced element waits for a rehash
		if (oldElement.tag != static_cast<IntPtr>(-1))
		{
			nonResidentEntries.push_back(oldElement);
			m_log->detailed("Tag ", oldElement.tag, " is non-resident until the next rehash");
		}
		return false;
	}

	/*
	 * Grow one page size's table to newSize and re-insert every element,
	 * then the non-resident ones.  If any re-insert fails the old tables
	 * are put back and the caller tries a larger size.
	 */
	bool PageTableCuckoo::rehash(int page_size_index, int newSize)
	{
		int oldSize = m_page_table_sizes[page_size_index];
		int oldItems = numItems[page_size_index];
		m_log->log("Rehashing page size ", m_page_size_list[page_size_index], " from ", oldSize, " to ", newSize, " elements (", oldItems, " tags)");

		Element **temp = tables[page_size_index];
		UInt64 *temp_ppns = table_ppns[page_size_index];

		Element **newTables = new Element *[m_ways];
		UInt64 *newPpns = new UInt64[m_ways];
		for (int i = 0; i < m_ways; i++)
		{
			newTables[i] = new Element[newSize];
			newPpns[i] = getPhysicalSpace(newSize * 64);
		}

		tables[page_size_index] = newTables;
		table_ppns[page_size_index] = newPpns;
		m_page_table_sizes[page_size_index] = newSize;
		// insertElement counts the tags it places; the count starts over
		numItems[page_size_index] = 0;

		std::vector<Element> savedNonResident = nonResidentEntries;

		auto revert = [&]() {
			for (int i = 0; i < m_ways; i++)
				delete[] newTables[i];
			delete[] newTables;
			delete[] newPpns;
			tables[page_size_index] = temp;
			table_ppns[page_size_index] = temp_ppns;
			m_page_table_sizes[page_size_index] = oldSize;
			numItems[page_size_index] = oldItems;
			nonResidentEntries = savedNonResident;
			m_log->log("Rehash to ", newSize, " failed");
		};

		for (int way = 0; way < m_ways; way++)
		{
			for (int pos = 0; pos < oldSize; pos++)
			{
				const Element &element = temp[way][pos];
				if (element.tag == static_cast<IntPtr>(-1))
					continue;
				for (int i = 0; i < 8; i++)
				{
					if (!element.validityBits[i])
						continue;
					uint64_t address = ((element.tag << 3) + i) << m_page_size_list[page_size_index];
					if (!insertElement(page_size_index, element, address))
					{
						revert();
						return false;
					}
				}
			}
		}

		nonResidentEntries.clear();
		for (const Element &element : savedNonResident)
		{
			for (int i = 0; i < 8; i++)
			{
				if (!element.validityBits[i])
					continue;
				uint64_t address = ((element.tag << 3) + i) << m_page_size_list[page_size_index];
				if (!insertElement(page_size_index, element, address))
				{
					revert();
					return false;
				}
			}
		}

		for (int i = 0; i < m_ways; i++)
			delete[] temp[i];
		delete[] temp;
		delete[] temp_ppns;

		cuckoo_stats.rehashes++;
		m_log->log("Rehash done: ", numItems[page_size_index], " tags");
		return true;
	}

	double PageTableCuckoo::currentLoadFactor(int page_size_index) const
	{
		return (double)numItems[page_size_index] / (m_page_table_sizes[page_size_index] * m_ways);
	}

	/*
	 * Read the tag's element in every way of every page size's table, all
	 * at depth 0: the reads are independent and go out together.  The
	 * accesses returned are those of the page size that resolved the
	 * translation (the page size is taken as known, as the released code
	 * does); the Cuckoo Walk Cache then picks the way.
	 */
	PTWResult PageTableCuckoo::initializeWalk(IntPtr address, bool count, bool is_prefetch, bool restart_walk_after_fault)
	{
		m_log->section("Cuckoo Walk");
		m_log->detailed("Walking for address ", SimLog::hex(address));

		if (count)
			cuckoo_stats.page_walks_total++;

		accessedAddresses visitedAddresses;
		bool is_page_fault_in_every_page_size = false;
		int page_size_result = -1;
		IntPtr ppn_result = 0;

	restart_walk:

		for (int i = 0; i < m_page_sizes; i++)
		{
			IntPtr VPN = address >> m_page_size_list[i];
			IntPtr tag = VPN >> 3;
			IntPtr offset = VPN % 8;

			for (int a = 0; a < m_ways; ++a)
			{
				uint64_t pos = hash(tag + a, m_page_table_sizes[i]);
				const Element &slot = tables[i][a][pos];
				IntPtr element_address = (IntPtr)(table_ppns[i][a] * 4096 + pos * 64);

				if (slot.tag == tag && slot.validityBits[offset])
				{
					m_log->detailed("Hit in page size ", m_page_size_list[i], " way ", a, " slot ", pos, ": ppn ", slot.frames[offset]);
					ppn_result = slot.frames[offset];
					page_size_result = m_page_size_list[i];
					visitedAddresses.push_back(PTWAccess(i, 0, element_address, true));
					if (count)
						cuckoo_stats.cuckoo_hits_per_level[i]++;
				}
				else
				{
					visitedAddresses.push_back(PTWAccess(i, 0, element_address, false));
				}
				if (count)
					cuckoo_stats.cuckoo_accesses[i]++;
			}
		}

		if (page_size_result != -1)
		{
			int page_size_index = pageSizeIndex(page_size_result);
			visitedAddresses.erase(std::remove_if(visitedAddresses.begin(), visitedAddresses.end(),
			                                      [page_size_index](const PTWAccess &access) { return access.table_level != page_size_index; }),
			                       visitedAddresses.end());
		}
		else
		{
			m_log->detailed("Page fault for address ", SimLog::hex(address));
			is_page_fault_in_every_page_size = true;
			if (!is_prefetch) cuckoo_stats.page_faults++;
			else cuckoo_stats.prefetch_page_faults++;

			if (restart_walk_after_fault)
			{
				raisePageFaultFromWalk(this, address, core_id, 0);
				m_log->detailed("Page fault resolved, restarting the walk");
				goto restart_walk;
			}
			return PTWResult(page_size_result, visitedAddresses, ppn_result, SubsecondTime::Zero(), is_page_fault_in_every_page_size, 0);
		}

		if (count)
			accesses_per_vpn[address >> 12]++;

		m_log->detailed("Walk done: page size ", page_size_result, " ppn ", ppn_result);
		return PTWResult(page_size_result, visitedAddresses, ppn_result, SubsecondTime::Zero(), is_page_fault_in_every_page_size, 0);
	}

	/*
	 * Map address -> ppn at page_size: rehash first if the table is past its
	 * load factor, insert, and grow until the insert succeeds.  The PMD-CWT
	 * section of the address learns the page size.  No frames are consumed.
	 */
	int PageTableCuckoo::updatePageTableFrames(IntPtr address, IntPtr core_id, IntPtr ppn, int page_size, std::vector<UInt64> frames)
	{
		int page_size_index = pageSizeIndex(page_size);
		m_log->section("Cuckoo Update");
		m_log->detailed("Mapping ", SimLog::hex(address), " -> ppn ", ppn, " at page size ", page_size);

		if (currentLoadFactor(page_size_index) > loadFactor)
		{
			int new_size = m_page_table_sizes[page_size_index] * m_scale;
			while (!rehash(page_size_index, new_size))
				new_size *= m_scale;
		}

		Element entry;
		uint64_t VPN = address >> m_page_size_list[page_size_index];
		uint64_t offset = VPN % 8;
		entry.tag = VPN >> 3;
		entry.validityBits[offset] = true;
		entry.frames[offset] = ppn;

		if (!insertElement(page_size_index, entry, address))
		{
			int new_size = m_page_table_sizes[page_size_index] * m_scale;
			while (!rehash(page_size_index, new_size))
				new_size *= m_scale;
		}

		IntPtr offset_in_cwt = (address >> 24) & 0x3F;
		PmdCwtEntry *row = retrieveCWTrow(address);
		if (page_size_index == 0)
			row->section_header[offset_in_cwt].has_4kb_page = 1;
		else
			row->section_header[offset_in_cwt].has_2mb_page = 1;

		accesses_per_vpn[address >> 12] = 1;
		return 0;
	}

	/*
	 * Unmap the base page at address from whichever way holds its block;
	 * an element left with no valid page is freed.
	 */
	void PageTableCuckoo::deletePage(IntPtr address)
	{
		IntPtr VPN = address >> m_page_size_list[0];
		IntPtr tag = VPN >> 3;
		IntPtr indexInsideBlock = VPN % 8;

		for (int a = 0; a < m_ways; ++a)
		{
			uint64_t pos = hash(tag + a, m_page_table_sizes[0]);
			Element &slot = tables[0][a][pos];
			if (slot.tag != tag)
				continue;

			slot.validityBits[indexInsideBlock] = false;
			slot.frames[indexInsideBlock] = static_cast<IntPtr>(-1);

			bool any_valid = false;
			for (int j = 0; j < 8; j++)
				any_valid |= slot.validityBits[j];
			if (!any_valid)
			{
				slot.tag = static_cast<IntPtr>(-1);
				numItems[0]--;
			}
			break;
		}
	}

	PmdCwtEntry *PageTableCuckoo::retrieveCWTrow(IntPtr address)
	{
		IntPtr pmd_tag = address >> 30;
		auto it = pmd_cwt_entries.find(pmd_tag);
		if (it == pmd_cwt_entries.end())
			it = pmd_cwt_entries.emplace(pmd_tag, PmdCwtEntry(pmd_tag)).first;
		return &it->second;
	}

} // namespace ParametricDramDirectoryMSI
