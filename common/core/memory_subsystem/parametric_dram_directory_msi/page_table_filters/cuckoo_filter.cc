#include "cuckoo_filter.h"
#include "cwc.h"
#include "config.hpp"
#include "simulator.h"
#include "core.h"
#include "pagetable.h"
#include "pagetable_cuckoo.h"
#include "debug_config.h"

namespace ParametricDramDirectoryMSI
{
	CuckooFilter::CuckooFilter(String _name, Core *_core)
		: BaseFilter(_name, _core)
	{
		int cwc_entries = Sim()->getCfg()->getInt("perf_model/" + _name + "/cwc/entries");
		cwc = new CWCache(_name + "_cwc", _core, cwc_entries);
		m_log = new SimLog(std::string(_name.c_str()) + "_cwc", _core->getId(), DEBUG_MMU);
		m_log->log("Cuckoo walk cache with ", cwc_entries, " entries");
	}

	CuckooFilter::~CuckooFilter()
	{
		delete cwc;
		delete m_log;
	}

	PTWResult CuckooFilter::filterPTWResult(IntPtr virtual_address, PTWResult ptw_result, PageTable *page_table, bool count)
	{
		// The CWT is the cuckoo table's; any other page table walks unfiltered.
		PageTableCuckoo *cuckoo_page_table = dynamic_cast<PageTableCuckoo *>(page_table);
		if (cuckoo_page_table == NULL)
			return ptw_result;

		// A row covers 1 GB (the tag), a section header 16 MB of it.
		IntPtr tag = virtual_address >> 30;
		IntPtr offset_pointer = (virtual_address >> 24) & 0x3F;
		m_log->trace("Filtering the walk of ", SimLog::hex(virtual_address), ": tag ", tag, " section ", offset_pointer);

		CWCRow cwc_row;
		bool hit_cwc = cwc->lookup(tag, cwc_row);
		const accessedAddresses &original_ptw_accesses = ptw_result.accesses;
		accessedAddresses filtered_ptw_accesses;

		// The page size and way that delivered the translation: the accesses
		// of one page size's table are its ways in order.
		int current_page_size = 0;
		int current_way = 0;
		int resolved_page_size = 0;
		int resolved_way = 0;
		for (const auto &access : original_ptw_accesses)
		{
			if (access.table_level != current_page_size)
			{
				current_page_size = access.table_level;
				current_way = 0;
			}
			if (access.is_pte)
			{
				resolved_page_size = (access.table_level == 0) ? 12 : 21;
				resolved_way = current_way;
			}
			current_way++;
		}

		if (!hit_cwc)
		{
			// Install the section's row; this walk goes out unfiltered.
			cwc->insert(CWCRow(cuckoo_page_table->retrieveCWTrow(virtual_address)));
			m_log->trace("CWC miss, row installed");
			return ptw_result;
		}

		CwtSectionHeader &header = cwc_row.cwt_entry_ptr->section_header[offset_pointer];

		if (header.has_4kb_page && header.has_2mb_page)
		{
			// Both tables: every way of the 4 KB table, the known way of the 2 MB one.
			int way_counter = 0;
			for (const auto &access : original_ptw_accesses)
			{
				if (access.table_level == 1)
				{
					if (way_counter == header.way)
						filtered_ptw_accesses.push_back(access);
					way_counter++;
				}
				else if (access.table_level == 0)
					filtered_ptw_accesses.push_back(access);
			}
		}
		else if (!header.has_4kb_page && header.has_2mb_page)
		{
			int way_counter = 0;
			for (const auto &access : original_ptw_accesses)
			{
				if (access.table_level == 1)
				{
					if (way_counter == header.way)
						filtered_ptw_accesses.push_back(access);
					way_counter++;
				}
			}
		}
		else if (header.has_4kb_page && !header.has_2mb_page)
		{
			for (const auto &access : original_ptw_accesses)
				if (access.table_level == 0)
					filtered_ptw_accesses.push_back(access);
		}
		else
		{
			// The section is new to the CWT: this walk teaches it.
			header.has_4kb_page = (resolved_page_size == 12);
			header.has_2mb_page = (resolved_page_size == 21);
			header.way = resolved_way;
			m_log->trace("CWC hit on an empty section, learned page size ", resolved_page_size, " way ", resolved_way);
			return ptw_result;
		}

		m_log->trace("CWC hit: ", original_ptw_accesses.size(), " accesses filtered to ", filtered_ptw_accesses.size());
		return PTWResult(ptw_result.page_size, filtered_ptw_accesses, ptw_result.ppn, ptw_result.pwc_latency, ptw_result.fault_happened, ptw_result.requested_frames);
	}
}
