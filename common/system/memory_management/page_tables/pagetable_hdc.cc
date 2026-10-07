#include "pagetable_hdc.h"
#include "pagetable_fault.h"
#include "simulator.h"
#include "config.hpp"
#include "city.h"
#include "memory_management/physical_memory_allocators/physical_memory_allocator.h"
#include "mimicos.h"
#include "debug_config.h"

#include <cstdlib>
#include <cassert>

namespace ParametricDramDirectoryMSI
{
	static MimicOS *osForPageTable(bool is_guest)
	{
		return is_guest ? Sim()->getMimicOS_VM() : Sim()->getMimicOS();
	}

	UInt64 PageTableHDC::hashFunction(IntPtr address, int table_size)
	{
		return CityHash64((const char *)&address, 8) % table_size;
	}

	IntPtr PageTableHDC::tableBytes(int size)
	{
		return osForPageTable(is_guest)->getMemoryAllocator()->handle_page_table_allocations(size * 64) * 4096;
	}

	int PageTableHDC::pageSizeIndex(int page_size) const
	{
		for (int i = 0; i < m_page_sizes; i++)
			if (m_page_size_list[i] == page_size)
				return i;
		return 0;
	}

	PageTableHDC::PageTableHDC(int core_id, String name, String type, int page_sizes, int *page_size_list, int *page_table_sizes, bool is_guest)
		: PageTable(core_id, name, type, page_sizes, page_size_list, is_guest),
		  m_page_table_sizes(page_table_sizes),
		  m_resize_threshold(0.6),
		  m_resize_factor(2)
	{
		m_log = new SimLog(std::string(name.c_str()) + "_hdc", core_id, DEBUG_PAGE_TABLE_HASH);

		String cfg = "perf_model/" + osForPageTable(is_guest)->getName() + "/" + name;
		if (Sim()->getCfg()->hasKey(cfg + "/resize_threshold"))
			m_resize_threshold = Sim()->getCfg()->getFloat(cfg + "/resize_threshold");
		if (Sim()->getCfg()->hasKey(cfg + "/resize_factor"))
			m_resize_factor = Sim()->getCfg()->getInt(cfg + "/resize_factor");

		m_entry_count = new int[page_sizes]();
		emulated_table_address = new IntPtr[m_page_sizes];

		for (int i = 0; i < m_page_sizes; i++)
		{
			emulated_table_address[i] = tableBytes(m_page_table_sizes[i]);
			m_log->log("Page table for page size ", m_page_size_list[i], ": ", m_page_table_sizes[i],
			           " entries (", m_page_table_sizes[i] * 64 / 1024, " KB) at ", SimLog::hex(emulated_table_address[i]));

			Entry *page_table = (Entry *)malloc(sizeof(Entry) * m_page_table_sizes[i]);
			for (int j = 0; j < m_page_table_sizes[i]; j++)
			{
				for (int k = 0; k < 8; k++)
				{
					page_table[j].valid[k] = false;
					page_table[j].ppn[k] = 0;
				}
				page_table[j].tag = static_cast<IntPtr>(-1);
				page_table[j].distance_from_root = -1;
			}
			page_tables.push_back(page_table);
		}

		stats.page_table_walks = new UInt64[m_page_sizes]();
		stats.num_accesses = new UInt64[m_page_sizes]();
		stats.collisions = new UInt64[m_page_sizes]();
		stats.resizes = new UInt64[m_page_sizes]();
		stats.page_faults = 0;
		stats.prefetch_page_faults = 0;

		for (int i = 0; i < m_page_sizes; i++)
		{
			String suffix = itostr(m_page_size_list[i]);
			registerStatsMetric(name, core_id, "page_table_walks_" + suffix, &stats.page_table_walks[i]);
			registerStatsMetric(name, core_id, "accesses_" + suffix, &stats.num_accesses[i]);
			registerStatsMetric(name, core_id, "collisions_" + suffix, &stats.collisions[i]);
			registerStatsMetric(name, core_id, "resizes_" + suffix, &stats.resizes[i]);
		}
		registerStatsMetric(name, core_id, "page_faults", &stats.page_faults);
		registerStatsMetric(name, core_id, "prefetch_page_faults", &stats.prefetch_page_faults);
	}

	PageTableHDC::~PageTableHDC()
	{
		for (int i = 0; i < m_page_sizes; i++)
			free(page_tables[i]);
		delete[] emulated_table_address;
		delete[] m_entry_count;
		delete m_log;
	}

	/*
	 * Probe every page size's table from the tag's home slot: a matching tag
	 * with the block offset valid is the translation, a matching tag without
	 * it or an empty slot ends the probe (unmapped), any other tag is a
	 * collision and the probe moves one slot on.  Every slot read is an
	 * access the MMU charges, at the page-size index and probe step.
	 */
	PTWResult PageTableHDC::initializeWalk(IntPtr address, bool count, bool is_prefetch, bool restart_walk_after_fault)
	{
		m_log->section("HDC Walk");
		m_log->detailed("Walking for address ", SimLog::hex(address));

		accessedAddresses visited_addresses;
		bool is_pagefault_in_every_page_size = false;

		if (count)
			for (int i = 0; i < m_page_sizes; i++)
				stats.page_table_walks[i]++;

	restart_walk:;

		int page_size_result = -1;
		IntPtr ppn = 0;

		for (int i = 0; i < m_page_sizes; i++)
		{
			IntPtr VPN = address >> m_page_size_list[i];
			IntPtr tag = VPN >> 3;
			IntPtr block_offset = VPN % 8;
			UInt64 slot = hashFunction(tag, m_page_table_sizes[i]);
			Entry *current_entry = &page_tables[i][slot];

			m_log->detailed("Page size ", m_page_size_list[i], ": VPN ", VPN, " tag ", tag, " home slot ", slot);

			int depth = 0;
			while (depth < m_page_table_sizes[i])
			{
				IntPtr entry_address = emulated_table_address[i] + 64 * slot + block_offset * 8;

				if (current_entry->tag == tag && current_entry->valid[block_offset])
				{
					m_log->detailed("Hit at probe ", depth, ": ppn ", current_entry->ppn[block_offset]);
					if (count)
						stats.num_accesses[i]++;
					ppn = current_entry->ppn[block_offset];
					page_size_result = m_page_size_list[i];
					visited_addresses.emplace_back(PTWAccess(i, depth, entry_address, true));
					break;
				}
				else if (current_entry->tag == static_cast<IntPtr>(-1) || current_entry->tag == tag)
				{
					// Empty, or the block is known and the page in it is not mapped.
					if (count)
						stats.num_accesses[i]++;
					visited_addresses.emplace_back(PTWAccess(i, depth, entry_address, false));
					break;
				}
				else
				{
					if (count)
					{
						stats.num_accesses[i]++;
						stats.collisions[i]++;
					}
					visited_addresses.emplace_back(PTWAccess(i, depth, entry_address, false));
					slot = (slot + 1) % m_page_table_sizes[i];
					current_entry = &page_tables[i][slot];
				}
				depth++;
			}

			if (depth >= m_page_table_sizes[i])
			{
				std::cerr << "[FATAL] [" << name << "] The page table for page size " << m_page_size_list[i] << " is full" << std::endl;
				exit(1);
			}
		}

		if (page_size_result == -1)
		{
			m_log->detailed("Page fault for address ", SimLog::hex(address));
			is_pagefault_in_every_page_size = true;
			if (!is_prefetch) stats.page_faults++;
			else stats.prefetch_page_faults++;

			if (restart_walk_after_fault)
			{
				raisePageFaultFromWalk(this, address, core_id, 0);
				m_log->detailed("Page fault resolved, restarting the walk");
				goto restart_walk;
			}
			return PTWResult(page_size_result, visited_addresses, ppn, SubsecondTime::Zero(), is_pagefault_in_every_page_size, 0);
		}

		if (count)
			accesses_per_vpn[address >> 12]++;

		m_log->detailed("Walk done: page size ", page_size_result, " ppn ", ppn);
		return PTWResult(page_size_result, visited_addresses, ppn, SubsecondTime::Zero(), is_pagefault_in_every_page_size, 0);
	}

	/*
	 * Map address -> ppn at page_size: probe from the home slot for an empty
	 * slot or the block's own entry.  No frames are consumed.
	 */
	int PageTableHDC::updatePageTableFrames(IntPtr address, IntPtr core_id, IntPtr ppn, int page_size, std::vector<UInt64> frames)
	{
		int page_size_index = pageSizeIndex(page_size);
		IntPtr VPN = address >> m_page_size_list[page_size_index];
		IntPtr tag = VPN >> 3;
		IntPtr block_offset = VPN % 8;
		UInt64 slot = hashFunction(tag, m_page_table_sizes[page_size_index]);
		Entry *current_entry = &page_tables[page_size_index][slot];

		m_log->section("HDC Update");
		m_log->detailed("Mapping ", SimLog::hex(address), " -> ppn ", ppn, " at page size ", page_size, " home slot ", slot);

		int depth = 0;
		while (depth < m_page_table_sizes[page_size_index])
		{
			if (current_entry->tag == static_cast<IntPtr>(-1))
			{
				current_entry->tag = tag;
				current_entry->valid[block_offset] = true;
				current_entry->ppn[block_offset] = ppn;
				current_entry->distance_from_root = depth;
				m_entry_count[page_size_index]++;

				double occupancy = static_cast<double>(m_entry_count[page_size_index]) / m_page_table_sizes[page_size_index];
				if (occupancy >= m_resize_threshold)
					resizeTable(page_size_index);
				break;
			}
			else if (current_entry->tag == tag)
			{
				current_entry->valid[block_offset] = true;
				current_entry->ppn[block_offset] = ppn;
				break;
			}
			else
			{
				slot = (slot + 1) % m_page_table_sizes[page_size_index];
				current_entry = &page_tables[page_size_index][slot];
			}
			depth++;
		}

		accesses_per_vpn[address >> 12] = 1;
		return 0;
	}

	/*
	 * Unmap the base page at address: probe for its block and clear the
	 * page's valid bit.  The slot keeps its tag, so later probes still pass
	 * through it.
	 */
	void PageTableHDC::deletePage(IntPtr address)
	{
		IntPtr VPN = address >> m_page_size_list[0];
		IntPtr tag = VPN >> 3;
		IntPtr block_offset = VPN % 8;
		UInt64 slot = hashFunction(tag, m_page_table_sizes[0]);
		Entry *current_entry = &page_tables[0][slot];

		for (int depth = 0; depth < m_page_table_sizes[0]; depth++)
		{
			if (current_entry->tag == tag)
			{
				current_entry->valid[block_offset] = false;
				break;
			}
			if (current_entry->tag == static_cast<IntPtr>(-1))
				break;
			slot = (slot + 1) % m_page_table_sizes[0];
			current_entry = &page_tables[0][slot];
		}
	}

	/*
	 * Grow one page size's table by the resize factor and rehash every
	 * occupied slot into it by linear probing.
	 */
	void PageTableHDC::resizeTable(int page_size_index)
	{
		int old_size = m_page_table_sizes[page_size_index];
		int new_size = old_size * m_resize_factor;

		m_log->log("Resizing the table for page size ", m_page_size_list[page_size_index], " from ", old_size, " to ", new_size,
		           " (", m_entry_count[page_size_index], " slots occupied)");

		Entry *new_table = (Entry *)malloc(sizeof(Entry) * new_size);
		for (int j = 0; j < new_size; j++)
		{
			for (int k = 0; k < 8; k++)
			{
				new_table[j].valid[k] = false;
				new_table[j].ppn[k] = 0;
			}
			new_table[j].tag = static_cast<IntPtr>(-1);
			new_table[j].distance_from_root = -1;
		}
		IntPtr new_pa = tableBytes(new_size);

		Entry *old_table = page_tables[page_size_index];
		for (int j = 0; j < old_size; j++)
		{
			if (old_table[j].tag == static_cast<IntPtr>(-1))
				continue;

			UInt64 idx = hashFunction(old_table[j].tag, new_size);
			for (int depth = 0; depth < new_size; depth++)
			{
				if (new_table[idx].tag == static_cast<IntPtr>(-1))
				{
					new_table[idx].tag = old_table[j].tag;
					new_table[idx].distance_from_root = depth;
					for (int k = 0; k < 8; k++)
					{
						new_table[idx].valid[k] = old_table[j].valid[k];
						new_table[idx].ppn[k] = old_table[j].ppn[k];
					}
					break;
				}
				idx = (idx + 1) % new_size;
			}
		}

		free(old_table);
		page_tables[page_size_index] = new_table;
		m_page_table_sizes[page_size_index] = new_size;
		emulated_table_address[page_size_index] = new_pa;
		stats.resizes[page_size_index]++;
	}

} // namespace ParametricDramDirectoryMSI
