#pragma once
#include "pagetable.h"
#include "pagetable_radix.h"
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

			return new PageTableRadix(app_id, name, type, page_sizes, page_size_list, levels, frame_size, is_guest);
		}

		static PageTable *createPageTable(String type, String name, UInt64 app_id, bool is_guest = false)
		{
			if (type == "radix")
			{
				String mimicos_name;

				mimicos_name = Sim()->getMimicOS()->getName();

				Core* core = Sim()->getCoreManager()->getCoreFromID(app_id);

				int page_sizes = Sim()->getCfg()->getInt("perf_model/" + mimicos_name + "/" + name + "/page_sizes");
				int *page_size_list = new int[page_sizes];

				for (int i = 0; i < page_sizes; i++)
				{
					page_size_list[i] = Sim()->getCfg()->getIntArray("perf_model/" + mimicos_name + "/" + name + "/page_size_list", i);
				}

				int levels = Sim()->getCfg()->getInt("perf_model/" + mimicos_name + "/" + name + "/levels");
				int frame_size = Sim()->getCfg()->getInt("perf_model/" + mimicos_name + "/" + name + "/frame_size");

				return createRadixPageTable(core->getId(), name, type, page_sizes, page_size_list, levels, frame_size, is_guest);
			}

			else
			{
				assert(0 && "Invalid page table name");
			}
		}
	};
}