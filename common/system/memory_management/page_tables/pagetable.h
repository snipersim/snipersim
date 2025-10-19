
#pragma once
#include "fixed_types.h"
#include "cache.h"
#include "simulator.h"
#include "core.h"
#include "core_manager.h"
#include <unordered_map>
#include <random>
#include "pwc.h"
#include <bitset>

// #define DEBUG
using namespace std;

namespace ParametricDramDirectoryMSI
{
	typedef std::vector<tuple<int, int, IntPtr, bool>> accessedAddresses;		  // <level of page table based on page size, depth of the page table, physical address we accessed, is it the PTE that contains the correct translation?>
	typedef tuple<int, accessedAddresses, IntPtr, SubsecondTime, bool> PTWResult; //<pagesize, addresses we accesses during walk, ppn, page walk cache latency (for intermediate levels), fault_happened?>
	typedef int PageSize;
	typedef tuple<int, accessedAddresses, IntPtr> AllocatedPage; //<pagesize, accessedAddresses, ppn>

	class PageTable
	{

	protected:
		int core_id;
		String name;
		String type;
		int *m_page_size_list;
		int m_page_sizes;
		Core *core;
		bool is_guest;

	public:
		PageTable(int core_id, String name, String type, int page_sizes, int *page_size_list, bool is_guest = false)
		{
			this->m_page_sizes = page_sizes;
			this->m_page_size_list = page_size_list;
			this->core_id = core_id;
			this->name = name;
			this->is_guest = is_guest;
			this->type = type;
		};

		virtual PTWResult initializeWalk(IntPtr address, bool count, bool is_prefetch = false, bool restart_walk = false) = 0;
		// virtual AllocatedPage handlePageFault(IntPtr address, bool count, IntPtr ppn = -1, int page_size = 12) = 0;
		int *getPageSizes() { return m_page_size_list; };
		int getPageSizesCount() { return m_page_sizes; };
		virtual int getMaxLevel() { return -1; }; // This function should be overriden by the derived class (e.g., in RadixPageTable there are maximum 4 levels)
		String getName() { return name; };
		String getType() { return type; };
		virtual void deletePage(IntPtr address) {};
		virtual int updatePageTableFrames(IntPtr address, IntPtr core_id, IntPtr ppn, int page_size, std::vector<UInt64> frames) = 0;
	};
}
