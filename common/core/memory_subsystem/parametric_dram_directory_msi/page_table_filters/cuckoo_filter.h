#pragma once
#include "base_filter.h"
#include "cwc.h"
#include "core.h"
#include "sim_log.h"

namespace ParametricDramDirectoryMSI
{
	/**
	 * @brief The PTW filter of the elastic cuckoo page table: a Cuckoo Walk
	 *        Cache over the PMD-CWT [Skarlatos et al., ASPLOS 2020].
	 *
	 * A walk of the cuckoo table probes every way of every page size's
	 * table.  The CWT knows, per 16 MB section, which page sizes are
	 * present and which way of the 2 MB table holds the section, so a walk
	 * whose section the CWC caches is cut down to the probes that can
	 * resolve it.  A CWC miss installs the section's row and leaves the walk
	 * as it is.
	 */
	class CuckooFilter : public BaseFilter
	{
	public:
		CuckooFilter(String name, Core *core);
		~CuckooFilter();

		PTWResult filterPTWResult(IntPtr virtual_address, PTWResult ptw_result, PageTable *page_table, bool count);

	private:
		CWCache *cwc;
		SimLog *m_log;
	};
}
