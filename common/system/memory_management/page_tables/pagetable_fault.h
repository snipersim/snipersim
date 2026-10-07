#pragma once
#include "pagetable.h"
#include "simulator.h"
#include "core.h"
#include "core_manager.h"
#include "misc/exception_handler_base.h"

namespace ParametricDramDirectoryMSI
{
	/**
	 * @brief Raise a page fault from inside a walk that restarts itself.
	 *
	 * A walk asked to restart after a fault (prefetch walks, the perfect
	 * translation path) resolves the fault in place: the core's exception
	 * handler maps the page, and the walk starts over.  The handler is
	 * reached through its base interface with a generic fault context, as
	 * the MMUs reach it, so any handler the configuration selects serves.
	 * metadata_frames is how many page-table frames the fault needs the
	 * handler to allocate: a hashed page table grows out of its own
	 * allocations and asks for none.
	 */
	inline void raisePageFaultFromWalk(PageTable *page_table, IntPtr address, int core_id, int metadata_frames)
	{
		if (Sim()->getCfg()->getBool("general/enable_userspace_mimicos"))
		{
			std::cerr << "[FATAL] [" << page_table->getName() << "] The userspace kernel resolved this fault before the replay; "
			          << "no fault may occur again for address " << address << std::endl;
			exit(1);
		}

		ExceptionHandlerBase *handler = Sim()->getCoreManager()->getCoreFromID(core_id)->getExceptionHandler();
		ExceptionHandlerBase::FaultCtx fault_ctx{};
		fault_ctx.vpn = address >> 12;
		fault_ctx.page_table = page_table;
		fault_ctx.alloc_in.metadata_frames = metadata_frames;
		fault_ctx.alloc_in.is_instruction = false;
		handler->handle_page_fault(fault_ctx);
	}
}
