#include "utopia_exception_handler.h"
#include "core.h"
#include "simulator.h"
#include "config.hpp"
#include "mimicos.h"
#include "stats.h"
#include "debug_config.h"
#include "memory_management/physical_memory_allocators/utopia.h"

UtopiaExceptionHandler::UtopiaExceptionHandler(Core *core)
    : SniperExceptionHandler(core),
      m_restseg_faults(0),
      m_flexseg_faults(0)
{
    String mmu_type = Sim()->getCfg()->getString("perf_model/mmu/type");
    m_map_restseg_in_page_table = !(mmu_type == "utopia" || mmu_type == "utopia_coalesce");
    std::cout << "[UTOPIA_EXCEPTION_HANDLER] Initializing Utopia Exception Handler for core " << core->getId()
              << (m_map_restseg_in_page_table ? " (MMU without a RestSeg walker: RestSeg pages are mapped in the page table too)"
                                              : " (RestSeg pages skip the page table)") << std::endl;
    registerStatsMetric("utopia_handler", core->getId(), "restseg_faults", &m_restseg_faults);
    registerStatsMetric("utopia_handler", core->getId(), "flexseg_faults", &m_flexseg_faults);
}

UtopiaExceptionHandler::~UtopiaExceptionHandler()
{
}

void UtopiaExceptionHandler::handle_page_fault(FaultCtx &ctx)
{
    ScopedLock sl(ExceptionHandlerBase::s_page_fault_lock);

    int core_id = this->m_core->getId();
    UInt64 fault_address = ctx.vpn << BASE_PAGE_SHIFT;

    PhysicalMemoryAllocator *allocator = this->allocatorFor(ctx);
    assert(allocator != NULL);

    // 1. The frame, through the Utopia hierarchy (RestSeg-2MB, FlexSeg-THP,
    //    RestSeg-4KB, FlexSeg-4KB); instruction pages go to the instruction area
    const auto &[ppn, page_size] = allocator->allocate(FOUR_KIB, fault_address, core_id, false, ctx.alloc_in.is_instruction);
    ctx.alloc_out.ppn = ppn;
    ctx.alloc_out.page_size = page_size;

    Utopia *utopia = dynamic_cast<Utopia *>(allocator);
    bool in_restseg = utopia && utopia->getLastAllocatedInRestSeg();

#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    std::cout << "[UTOPIA_EXCEPTION_HANDLER] Fault at " << std::hex << fault_address << std::dec << " core " << core_id
              << ": ppn " << ppn << " page size " << page_size << (in_restseg ? " in RestSeg" : " in FlexSeg") << std::endl;
#endif

    // 2. Page table entries for FlexSeg pages; RestSeg pages are translated by
    //    the RestSeg walker and get none, unless the MMU has no such walker
    if (in_restseg)
        m_restseg_faults++;
    else
        m_flexseg_faults++;
    if (!in_restseg || m_map_restseg_in_page_table)
        allocate_page_table_frames(ctx, fault_address, core_id, ppn, page_size, ctx.alloc_in.metadata_frames);
    else
        ctx.alloc_out.prealloc_frames.clear();
}
