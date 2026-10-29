#include "pagetable_ht.h"
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
	/*
	 * The MimicOS of this page table: the guest's for a guest (VM) page table,
	 * the host's otherwise.  Its allocator carves the tables and the chained
	 * entries out of the kernel region.
	 */
	static MimicOS *osForPageTable(bool is_guest)
	{
		return is_guest ? Sim()->getMimicOS_VM() : Sim()->getMimicOS();
	}

	UInt64 PageTableHT::hashFunction(IntPtr address, int table_size)
	{
		return CityHash64((const char *)&address, 8) % table_size;
	}

	/*
	 * handle_page_table_allocations returns a frame number; the entries are
	 * addressed in bytes, 64 per entry.
	 */
	IntPtr PageTableHT::tableBytes(int size)
	{
		return osForPageTable(is_guest)->getMemoryAllocator()->handle_page_table_allocations(size * 64) * 4096;
	}

	int PageTableHT::pageSizeIndex(int page_size) const
	{
		for (int i = 0; i < m_page_sizes; i++)
			if (m_page_size_list[i] == page_size)
				return i;
		return 0;
	}

	PageTableHT::PageTableHT(int core_id, String name, String type, int page_sizes, int *page_size_list, int *page_table_sizes, bool is_guest)
		: PageTable(core_id, name, type, page_sizes, page_size_list, is_guest),
		  m_page_table_sizes(page_table_sizes),
		  m_resize_threshold(0.6),
		  m_resize_factor(2)
	{
		m_log = new SimLog(std::string(name.c_str()) + "_ht", core_id, DEBUG_PAGE_TABLE_HASH);

		// The page table's own keys sit beside the radix table's, under the OS
		// that owns it.
		String cfg = "perf_model/" + osForPageTable(is_guest)->getName() + "/" + name;
		if (Sim()->getCfg()->hasKey(cfg + "/resize_threshold"))
			m_resize_threshold = Sim()->getCfg()->getFloat(cfg + "/resize_threshold");
		if (Sim()->getCfg()->hasKey(cfg + "/resize_factor"))
			m_resize_factor = Sim()->getCfg()->getInt(cfg + "/resize_factor");

		m_entry_count = new int[page_sizes]();
		table_pa = new IntPtr[m_page_sizes];

		for (int i = 0; i < m_page_sizes; i++)
		{
			Entry *page_table = (Entry *)malloc(sizeof(Entry) * m_page_table_sizes[i]);
			table_pa[i] = tableBytes(m_page_table_sizes[i]);

			m_log->log("Page table for page size ", m_page_size_list[i], ": ", m_page_table_sizes[i],
			           " root entries at ", SimLog::hex(table_pa[i]));

			for (int j = 0; j < m_page_table_sizes[i]; j++)
			{
				page_table[j].tag = static_cast<IntPtr>(-1);
				page_table[j].next_entry = NULL;
				for (int k = 0; k < 8; k++)
				{
					page_table[j].valid[k] = false;
					page_table[j].ppn[k] = 0;
				}
				page_table[j].emulated_physical_address = table_pa[i] + j * 64;
			}
			page_tables.push_back(page_table);
		}

		stats.page_table_walks = new UInt64[m_page_sizes]();
		stats.chained = new UInt64[m_page_sizes]();
		stats.num_accesses = new UInt64[m_page_sizes]();
		stats.resizes = new UInt64[m_page_sizes]();
		stats.page_faults = 0;
		stats.prefetch_page_faults = 0;

		for (int i = 0; i < m_page_sizes; i++)
		{
			String suffix = itostr(m_page_size_list[i]);
			registerStatsMetric(name, core_id, "page_table_walks_" + suffix, &stats.page_table_walks[i]);
			registerStatsMetric(name, core_id, "conflicts_" + suffix, &stats.chained[i]);
			registerStatsMetric(name, core_id, "accesses_" + suffix, &stats.num_accesses[i]);
			registerStatsMetric(name, core_id, "resizes_" + suffix, &stats.resizes[i]);
		}
		registerStatsMetric(name, core_id, "page_faults", &stats.page_faults);
		registerStatsMetric(name, core_id, "prefetch_page_faults", &stats.prefetch_page_faults);
	}

	PageTableHT::~PageTableHT()
	{
		for (int i = 0; i < m_page_sizes; i++)
		{
			for (int j = 0; j < m_page_table_sizes[i]; j++)
			{
				Entry *chained = page_tables[i][j].next_entry;
				while (chained != NULL)
				{
					Entry *next = chained->next_entry;
					free(chained);
					chained = next;
				}
			}
			free(page_tables[i]);
		}
		delete[] table_pa;
		delete[] m_entry_count;
		delete m_log;
	}

	/*
	 * Probe every page size's table for the address: hash the tag to a root
	 * slot and follow its chain until the tag matches (a translation if the
	 * block offset is valid, otherwise the page is unmapped), the chain ends,
	 * or an empty slot is reached.  Every entry touched is an access the MMU
	 * charges, at the page-size index (table_level) and chain step (depth).
	 */
	PTWResult PageTableHT::initializeWalk(IntPtr address, bool count, bool is_prefetch, bool restart_walk_after_fault)
	{
		m_log->section("HT Walk");
		m_log->detailed("Walking for address ", SimLog::hex(address));

		accessedAddresses visited_addresses;
		int page_size_result = -1;
		IntPtr ppn_result = 0;
		bool total_page_fault = false;

		if (count)
			for (int i = 0; i < m_page_sizes; i++)
				stats.page_table_walks[i]++;

	restart_walk:

		for (int i = 0; i < m_page_sizes; i++)
		{
			IntPtr VPN = address >> m_page_size_list[i];
			IntPtr tag = VPN >> 3;
			IntPtr block_offset = VPN % 8;
			UInt64 slot = hashFunction(tag, m_page_table_sizes[i]);

			Entry *current_entry = &page_tables[i][slot];
			int counter = 0;

			m_log->detailed("Page size ", m_page_size_list[i], ": VPN ", VPN, " tag ", tag, " offset ", block_offset, " slot ", slot);

			while (true)
			{
				IntPtr entry_address = current_entry->emulated_physical_address + block_offset * 8;

				if (current_entry->tag == tag && current_entry->valid[block_offset])
				{
					m_log->detailed("Hit: ppn ", current_entry->ppn[block_offset], " at ", SimLog::hex(entry_address));
					visited_addresses.push_back(PTWAccess(i, counter, entry_address, true));
					if (count)
						stats.num_accesses[i]++;
					page_size_result = m_page_size_list[i];
					ppn_result = current_entry->ppn[block_offset];
					break;
				}
				else if (current_entry->tag == tag)
				{
					// The block is known, the page in it is not mapped.
					visited_addresses.push_back(PTWAccess(i, counter, entry_address, false));
					if (count)
						stats.num_accesses[i]++;
					break;
				}
				else
				{
					// Another tag, or an empty root slot: the chain decides.
					visited_addresses.push_back(PTWAccess(i, counter, entry_address, false));
					if (count)
						stats.num_accesses[i]++;
					if (current_entry->next_entry == NULL)
						break;
					current_entry = current_entry->next_entry;
					counter++;
				}
			}
		}

		if (page_size_result == -1)
		{
			m_log->detailed("Page fault for address ", SimLog::hex(address));
			total_page_fault = true;
			if (!is_prefetch) stats.page_faults++;
			else stats.prefetch_page_faults++;

			if (restart_walk_after_fault)
			{
				raisePageFaultFromWalk(this, address, core_id, 0);
				m_log->detailed("Page fault resolved, restarting the walk");
				goto restart_walk;
			}
			return PTWResult(page_size_result, visited_addresses, ppn_result, SubsecondTime::Zero(), total_page_fault, 0);
		}

		if (count)
			countVPNAccess(address >> 12);

		m_log->detailed("Walk done: page size ", page_size_result, " ppn ", ppn_result);
		return PTWResult(page_size_result, visited_addresses, ppn_result, SubsecondTime::Zero(), total_page_fault, 0);
	}

	/*
	 * Map address -> ppn at page_size.  The root slot is filled if empty, the
	 * block's entry is extended if the tag is already there, and otherwise a
	 * new entry is chained from the kernel region.  No frames are consumed:
	 * the handler is asked for none, and the table grows out of its own
	 * allocations.
	 */
	int PageTableHT::updatePageTableFrames(IntPtr address, IntPtr core_id, IntPtr ppn, int page_size, std::vector<UInt64> frames)
	{
		int page_size_index = pageSizeIndex(page_size);
		IntPtr VPN = address >> m_page_size_list[page_size_index];
		IntPtr tag = VPN >> 3;
		IntPtr block_offset = VPN % 8;
		UInt64 slot = hashFunction(tag, m_page_table_sizes[page_size_index]);

		m_log->section("HT Update");
		m_log->detailed("Mapping ", SimLog::hex(address), " -> ppn ", ppn, " at page size ", page_size, " slot ", slot);

		Entry *current_entry = &page_tables[page_size_index][slot];

		while (true)
		{
			if (current_entry->tag == static_cast<IntPtr>(-1))
			{
				current_entry->tag = tag;
				for (int i = 0; i < 8; i++)
					current_entry->valid[i] = false;
				current_entry->valid[block_offset] = true;
				current_entry->ppn[block_offset] = ppn;
				current_entry->next_entry = NULL;
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
			else if (current_entry->next_entry == NULL)
			{
				Entry *new_entry = (Entry *)malloc(sizeof(Entry));
				new_entry->tag = tag;
				for (int i = 0; i < 8; i++)
				{
					new_entry->valid[i] = false;
					new_entry->ppn[i] = 0;
				}
				new_entry->valid[block_offset] = true;
				new_entry->ppn[block_offset] = ppn;
				new_entry->next_entry = NULL;
				new_entry->emulated_physical_address = osForPageTable(is_guest)->getMemoryAllocator()->handle_fine_grained_page_table_allocations(64);

				current_entry->next_entry = new_entry;
				stats.chained[page_size_index]++;
				m_log->detailed("Chained a new entry at ", SimLog::hex(new_entry->emulated_physical_address));
				break;
			}
			else
			{
				current_entry = current_entry->next_entry;
			}
		}

		setVPNAccesses(address >> 12, 1);
		return 0;
	}

	/*
	 * Unmap the base page at address.  An entry left with no valid page is
	 * emptied, and the first chained entry behind it, if any, takes its slot.
	 */
	void PageTableHT::deletePage(IntPtr address)
	{
		IntPtr VPN = address >> m_page_size_list[0];
		IntPtr tag = VPN >> 3;
		IntPtr block_offset = VPN % 8;
		UInt64 slot = hashFunction(tag, m_page_table_sizes[0]);
		Entry *current_entry = &page_tables[0][slot];

		m_log->detailed("Deleting the page of ", SimLog::hex(address), " tag ", tag, " offset ", block_offset);

		while (true)
		{
			if (current_entry->tag == tag)
			{
				current_entry->valid[block_offset] = false;
				current_entry->ppn[block_offset] = 0;

				bool any_valid = false;
				for (int i = 0; i < 8; i++)
					any_valid |= current_entry->valid[i];
				if (!any_valid)
				{
					current_entry->tag = static_cast<IntPtr>(-1);
					if (current_entry->next_entry != NULL)
					{
						Entry *temp = current_entry->next_entry;
						current_entry->tag = temp->tag;
						current_entry->next_entry = temp->next_entry;
						for (int j = 0; j < 8; j++)
						{
							current_entry->ppn[j] = temp->ppn[j];
							current_entry->valid[j] = temp->valid[j];
						}
						free(temp);
					}
				}
				break;
			}
			if (current_entry->next_entry == NULL)
				break;
			current_entry = current_entry->next_entry;
		}
	}

	/*
	 * Grow one page size's table by the resize factor: a new root array from
	 * the kernel region, every entry and chain rehashed into it, the old one
	 * freed.  Entries that collide in the new table are chained again.
	 */
	void PageTableHT::resizeTable(int page_size_index)
	{
		int old_size = m_page_table_sizes[page_size_index];
		int new_size = old_size * m_resize_factor;

		m_log->log("Resizing the table for page size ", m_page_size_list[page_size_index], " from ", old_size, " to ", new_size,
		           " (", m_entry_count[page_size_index], " root slots occupied)");

		Entry *new_table = (Entry *)malloc(sizeof(Entry) * new_size);
		IntPtr new_pa = tableBytes(new_size);
		for (int j = 0; j < new_size; j++)
		{
			new_table[j].tag = static_cast<IntPtr>(-1);
			new_table[j].next_entry = NULL;
			for (int k = 0; k < 8; k++)
			{
				new_table[j].valid[k] = false;
				new_table[j].ppn[k] = 0;
			}
			new_table[j].emulated_physical_address = new_pa + j * 64;
		}

		Entry *old_table = page_tables[page_size_index];
		int new_entry_count = 0;

		for (int j = 0; j < old_size; j++)
		{
			Entry *entry = &old_table[j];
			while (entry != NULL && entry->tag != static_cast<IntPtr>(-1))
			{
				IntPtr tag = entry->tag;
				Entry *dst = &new_table[hashFunction(tag, new_size)];

				if (dst->tag == static_cast<IntPtr>(-1))
				{
					dst->tag = tag;
					for (int k = 0; k < 8; k++)
					{
						dst->valid[k] = entry->valid[k];
						dst->ppn[k] = entry->ppn[k];
					}
					new_entry_count++;
				}
				else if (dst->tag == tag)
				{
					for (int k = 0; k < 8; k++)
						if (entry->valid[k])
						{
							dst->valid[k] = true;
							dst->ppn[k] = entry->ppn[k];
						}
				}
				else
				{
					Entry *new_chained = (Entry *)malloc(sizeof(Entry));
					new_chained->tag = tag;
					new_chained->next_entry = NULL;
					new_chained->emulated_physical_address = osForPageTable(is_guest)->getMemoryAllocator()->handle_fine_grained_page_table_allocations(64);
					for (int k = 0; k < 8; k++)
					{
						new_chained->valid[k] = entry->valid[k];
						new_chained->ppn[k] = entry->ppn[k];
					}
					Entry *tail = dst;
					while (tail->next_entry != NULL)
						tail = tail->next_entry;
					tail->next_entry = new_chained;
				}

				Entry *next = entry->next_entry;
				if (entry != &old_table[j])
					free(entry);
				entry = next;
			}
		}

		free(old_table);
		page_tables[page_size_index] = new_table;
		m_page_table_sizes[page_size_index] = new_size;
		table_pa[page_size_index] = new_pa;
		m_entry_count[page_size_index] = new_entry_count;
		stats.resizes[page_size_index]++;
	}

} // namespace ParametricDramDirectoryMSI
