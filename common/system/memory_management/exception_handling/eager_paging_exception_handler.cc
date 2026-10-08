#include "eager_paging_exception_handler.h"
#include "core.h"
#include "thread.h"
#include "simulator.h"
#include "core_manager.h"
#include "mimicos.h"
#include "stats.h"
#include "pagetable.h"
#include "rangetable.h"
#include "memory_management/physical_memory_allocators/physical_memory_allocator.h"
#include "memory_management/misc/vma.h"
#include "debug_config.h"

#include <cstring>
#include <algorithm>

EagerPagingExceptionHandler::EagerPagingExceptionHandler(Core *core) : ExceptionHandlerBase(core)
{
    m_log = new SimLog("EAGER_PAGING", core->getId(), DEBUG_EAGER_PAGING);
    memset(&m_stats, 0, sizeof(m_stats));
    registerStatsMetric("eager_paging", core->getId(), "vma_faults", &m_stats.vma_faults);
    registerStatsMetric("eager_paging", core->getId(), "pages_mapped", &m_stats.pages_mapped);
    registerStatsMetric("eager_paging", core->getId(), "no_vma_faults", &m_stats.no_vma_faults);
    std::cout << "[EXCEPTION_HANDLER] Initializing Eager Paging Exception Handler (sniper-space MimicOS) for core "
              << core->getId() << std::endl;
}

EagerPagingExceptionHandler::~EagerPagingExceptionHandler()
{
    delete m_log;
}

void EagerPagingExceptionHandler::handle_exception(int exception_type_code, int argc, uint64_t *argv)
{
    std::cout << "[EagerPagingExceptionHandler] handle_exception not implemented yet... exiting with status code 1" << std::endl;
    exit(1);
}

void EagerPagingExceptionHandler::handle_page_fault(FaultCtx &ctx)
{
    ScopedLock sl(ExceptionHandlerBase::s_page_fault_lock);

    UInt64 address = ctx.vpn << BASE_PAGE_SHIFT;
    int core_id = m_core->getId();
    Core *core_faulter = Sim()->getCoreManager()->getCoreFromID(core_id);
    Thread *thread_faulter = core_faulter ? core_faulter->getThread() : NULL;
    int app_id = thread_faulter ? thread_faulter->getAppId() : -1;
    m_log->debug("Page fault for", SimLog::hex(address), "on core", core_id, "app", app_id);

    std::vector<VMA> &vmas = Sim()->getMimicOS()->getVMA(app_id);
    if (vmas.empty())
    {
        std::cerr << "[EAGER_PAGING] *ERROR* no VMAs for application " << app_id
                  << ": eager paging lays a VMA out whole on its first fault, and the VMAs of an application "
                     "come only from the trace's VMA log. Record the guest with record-trace --vma and run "
                     "--traces=<trace>.sift with <trace>.vma beside it (a live run has no VMA information)."
                  << std::endl;
        exit(1);
    }

    for (VMA &vma : vmas)
    {
        if (!vma.contains(address))
            continue;
        if (vma.isAllocated())
        {
            // Laid out already: the page was mapped then, so this is a
            // page the layout could not cover (the allocator ran dry).
            // Map it alone.
            break;
        }
        allocate_vma_eagerly(ctx, vma, app_id);
        return;
    }

    // Outside every recorded VMA, or past a layout that ran dry: one page,
    // as demand paging would.
    m_stats.no_vma_faults++;
    const auto &[ppn, page_size] = getAllocator()->allocate(FOUR_KIB, address, core_id, false, ctx.alloc_in.is_instruction);
    ctx.alloc_out.ppn = ppn;
    ctx.alloc_out.page_size = page_size;
    map_page(ctx, address, ppn, page_size);
    m_log->debug("No VMA for", SimLog::hex(address), ": one page at frame", ppn);
}

void EagerPagingExceptionHandler::allocate_vma_eagerly(FaultCtx &ctx, VMA &vma, int app_id)
{
    m_log->debug("Laying out VMA", SimLog::hex(vma.getBase()), "-", SimLog::hex(vma.getEnd()));
    m_stats.vma_faults++;

    std::vector<Range> ranges = getAllocator()->allocate_ranges(vma.getBase(), vma.getEnd(), app_id);
    ParametricDramDirectoryMSI::RangeTable *range_table = Sim()->getMimicOS()->getRangeTable(app_id);
    IntPtr fault_address = ctx.vpn << BASE_PAGE_SHIFT;
    bool fault_mapped = false;

    for (const Range &range : ranges)
    {
        // Every page of the range into the page table, each at its frame.
        for (IntPtr j = 0; j < range.bounds; j++)
        {
            IntPtr page_address = (range.vpn + j) * FOUR_KIB;
            IntPtr ppn = range.offset + j;
            map_page(ctx, page_address, ppn, 12);
            if (page_address == fault_address)
            {
                ctx.alloc_out.ppn = ppn;
                ctx.alloc_out.page_size = 12;
                fault_mapped = true;
            }
        }
        m_stats.pages_mapped += range.bounds;

        // The range for the range MMU: [base, end) in bytes, frame of the base.
        vma.addPhysicalRange(range);
        if (range_table)
        {
            ParametricDramDirectoryMSI::RangeEntry entry;
            entry.offset = range.offset;
            range_table->insert(std::make_pair(range.vpn * FOUR_KIB, (range.vpn + range.bounds) * FOUR_KIB), entry);
        }
    }
    vma.setAllocated(true);
    m_log->debug("VMA laid out in", ranges.size(), "ranges");

    if (!fault_mapped)
    {
        // The layout stopped short of the faulting page (out of memory):
        // map that one page so the walk can complete.
        m_stats.no_vma_faults++;
        const auto &[ppn, page_size] = getAllocator()->allocate(FOUR_KIB, fault_address, m_core->getId(), false, ctx.alloc_in.is_instruction);
        ctx.alloc_out.ppn = ppn;
        ctx.alloc_out.page_size = page_size;
        map_page(ctx, fault_address, ppn, page_size);
    }
}

void EagerPagingExceptionHandler::map_page(FaultCtx &ctx, UInt64 address, UInt64 ppn, int page_size)
{
    // The page table may need frames of its own for the new entry.  The
    // fault's own count (alloc_in.metadata_frames) is what the faulting
    // page needs; the other pages of the VMA may cross into page-table
    // pages that do not exist yet, so every page is offered a whole
    // path's worth (the table's depth) and the frames it does not use go
    // back, as the sniper-space handler returns them.
    int core_id = m_core->getId();
    Core *core_faulter = Sim()->getCoreManager()->getCoreFromID(core_id);
    Thread *thread_faulter = core_faulter->getThread();
    int app_id = thread_faulter->getAppId();
    ParametricDramDirectoryMSI::PageTable *page_table = ctx.page_table ? ctx.page_table : Sim()->getMimicOS()->getPageTable(app_id);
    int requested = std::max(ctx.alloc_in.metadata_frames, page_table->getMaxLevel());
    std::vector<UInt64> frames;
    for (int i = 0; i < requested; i++)
    {
        UInt64 frame = getAllocator()->handle_page_table_allocations(FOUR_KIB, core_id);
        if (frame == static_cast<UInt64>(-1))
        {
            std::cerr << "[FATAL] [EAGER_PAGING] Out of memory while allocating page table frames" << std::endl;
            assert(false);
        }
        frames.push_back(frame);
    }

    int frames_used = page_table->updatePageTableFrames(address, core_id, ppn, page_size, frames);
    for (int i = 0; i < requested - frames_used; i++)
        getAllocator()->handle_page_table_deallocations(FOUR_KIB);
    ctx.alloc_out.prealloc_frames = frames;
}

PhysicalMemoryAllocator* EagerPagingExceptionHandler::getAllocator()
{
    if (m_allocator == NULL)
        m_allocator = Sim()->getMimicOS()->getMemoryAllocator();
    return m_allocator;
}
