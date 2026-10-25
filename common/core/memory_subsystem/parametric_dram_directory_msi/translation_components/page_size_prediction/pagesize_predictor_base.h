#pragma once
#include "cache_cntlr.h"
#include "subsecond_time.h"
#include "fixed_types.h"
#include "core.h"
#include "shmem_perf_model.h"

namespace ParametricDramDirectoryMSI
{
	class PageSizePredictorBase
	{

	protected:
		String name;		 // Name of the predictor
		Core *core;			 // Pointer to the core this predictor is associated with
		int page_sizes;		 // Number of different page sizes supported
		int *page_size_list; // Array of page sizes in bits (e.g., 12 for 4KB, 21 for 2MB)

	public:
		PageSizePredictorBase()
			: name(""), core(NULL), page_sizes(0), page_size_list(NULL)
		{
		}
		PageSizePredictorBase(String _name, Core *_core)
			: name(_name), core(_core)
		{
		}
		virtual ~PageSizePredictorBase() {}
		virtual int predictPageSize(IntPtr virtual_address) = 0;
		virtual void update(IntPtr virtual_address, int page_size) = 0;
	};
}
