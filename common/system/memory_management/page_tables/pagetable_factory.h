#pragma once
#include "pagetable.h"
#include "pagetable_radix.h"
// Step 13: the hashed page table with chaining
#include "pagetable_ht.h"
// Step 14: Hash, Don't Cache
#include "pagetable_hdc.h"
#include "config.hpp"
#include "mimicos.h"

namespace ParametricDramDirectoryMSI
{
	class PageTableFactory
	{
	public:
		static PageTable *createRadixPageTable(int app_id, String name, String type, int page_sizes, int *page_size_list, int levels, int frame_size, bool is_guest)
		{
			std::cout << "[Radix Page Table] Creating 4-level Radix table with name: " << name << "for app id: " << app_id << std::endl;
			std::cout << "[Radix Page Table] Page sizes: " << page_sizes << std::endl;
			for (int i = 0; i < page_sizes; i++)
			{
				std::cout << "[Page Table] Page size: " << page_size_list[i] << std::endl;
			}
			std::cout << "[Radix Page Table] Levels: " << levels << std::endl;
			std::cout << "[Radix Page Table] Frame size: " << frame_size << " entries" << std::endl;

			auto* pt = new PageTableRadix(app_id, name, type, page_sizes, page_size_list, levels, frame_size, is_guest);

			std::cout << "[Radix Page Table] createRadixPageTable created new PageTableRadix object with name: " << pt->getName() << " and type: " << pt->getType() << std::endl;
			return pt;
		}

		// The hashed page tables take one table size per page size; their keys
		// sit under the OS that owns them, beside the radix table's.
		static void readHashedPageTableConfig(String name, bool is_guest, int &page_sizes, int *&page_size_list, int *&page_table_size_list)
		{
			String mimicos_name = is_guest ? Sim()->getMimicOS_VM()->getName() : Sim()->getMimicOS()->getName();
			String cfg = "perf_model/" + mimicos_name + "/" + name;
			page_sizes = Sim()->getCfg()->getInt(cfg + "/page_sizes");
			page_size_list = new int[page_sizes];
			page_table_size_list = new int[page_sizes];
			for (int i = 0; i < page_sizes; i++)
			{
				page_size_list[i] = Sim()->getCfg()->getIntArray(cfg + "/page_size_list", i);
				page_table_size_list[i] = Sim()->getCfg()->getIntArray(cfg + "/page_table_size_list", i);
			}
		}

		// Step 13: the hashed page table with chaining
		static PageTable *createHTPageTable(int app_id, String name, String type, int page_sizes, int *page_size_list, int *page_table_size_list, bool is_guest)
		{
			std::cout << "[HT Page Table] Creating hashed page table with chaining " << name << " for app id: " << app_id << std::endl;
			for (int i = 0; i < page_sizes; i++)
				std::cout << "[HT Page Table] Page size " << page_size_list[i] << ": " << page_table_size_list[i] << " root entries" << std::endl;
			return new PageTableHT(app_id, name, type, page_sizes, page_size_list, page_table_size_list, is_guest);
		}

		// Step 14: Hash, Don't Cache
		static PageTable *createHDCPageTable(int app_id, String name, String type, int page_sizes, int *page_size_list, int *page_table_size_list, bool is_guest)
		{
			std::cout << "[HDC Page Table] Creating Hash, Don't Cache table [SIGMETRICS 2016] " << name << " for app id: " << app_id << std::endl;
			for (int i = 0; i < page_sizes; i++)
				std::cout << "[HDC Page Table] Page size " << page_size_list[i] << ": " << page_table_size_list[i] << " entries" << std::endl;
			return new PageTableHDC(app_id, name, type, page_sizes, page_size_list, page_table_size_list, is_guest);
		}

		static PageTable *createPageTable(String type, String name, UInt64 app_id, bool is_guest = false)
		{
			// Step 14: Hash, Don't Cache
			if (type == "hash_dont_cache")
			{
				int page_sizes, *page_size_list, *page_table_size_list;
				readHashedPageTableConfig(name, is_guest, page_sizes, page_size_list, page_table_size_list);
				return createHDCPageTable(app_id, name, type, page_sizes, page_size_list, page_table_size_list, is_guest);
			}
			// Step 13: the hashed page table with chaining
			if (type == "hash_table")
			{
				int page_sizes, *page_size_list, *page_table_size_list;
				readHashedPageTableConfig(name, is_guest, page_sizes, page_size_list, page_table_size_list);
				return createHTPageTable(app_id, name, type, page_sizes, page_size_list, page_table_size_list, is_guest);
			}
			if (type == "radix")
			{
				String mimicos_name;
				if (is_guest == true)
					mimicos_name = Sim()->getMimicOS_VM()->getName();
				else
					mimicos_name = Sim()->getMimicOS()->getName();

				int page_sizes = Sim()->getCfg()->getInt("perf_model/" + mimicos_name + "/" + name + "/page_sizes");
				int *page_size_list = new int[page_sizes];

				for (int i = 0; i < page_sizes; i++)
				{
					page_size_list[i] = Sim()->getCfg()->getIntArray("perf_model/" + mimicos_name + "/" + name + "/page_size_list", i);
				}

				int levels = Sim()->getCfg()->getInt("perf_model/" + mimicos_name + "/" + name + "/levels");
				int frame_size = Sim()->getCfg()->getInt("perf_model/" + mimicos_name + "/" + name + "/frame_size");

				return createRadixPageTable(app_id, name, type, page_sizes, page_size_list, levels, frame_size, is_guest);
			}
			else
				assert(0 && "Invalid page table type");
		}
	};
}
