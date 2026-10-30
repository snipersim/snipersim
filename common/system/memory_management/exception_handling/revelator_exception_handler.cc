
#include "revelator_exception_handler.h"
#include "core.h"
#include "simulator.h"
#include "mimicos.h"
#include "thread.h"
#include "debug_config.h"
#include "memory_management/physical_memory_allocators/physical_memory_allocator.h"

RevelatorExceptionHandler::RevelatorExceptionHandler(Core *core)
    : SniperExceptionHandler(core)
{
    std::cout << "[REVELATOR_EXCEPTION_HANDLER] Initializing Revelator Exception Handler for core " << core->getId() << std::endl;
    std::string log_name = "revelator_exception_handler.log." + std::to_string(core->getId());
    log_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + log_name;
    rev_log_file.open(log_name.c_str());
    rev_log_file << "[REVELATOR_EXCEPTION_HANDLER] Initialised from the Core constructor" << std::endl;
}

RevelatorExceptionHandler::~RevelatorExceptionHandler()
{
    if (rev_log_file.is_open())
        rev_log_file.close();
    std::cout << "[REVELATOR_EXCEPTION_HANDLER] destructor called" << std::endl;
}

void RevelatorExceptionHandler::handle_page_fault(FaultCtx &ctx)
{
    // Acquire global lock for thread-safe page fault handling in multi-core
    ScopedLock sl(ExceptionHandlerBase::s_page_fault_lock);

#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    std::cout << "[REVELATOR_EXCEPTION_HANDLER] Handling page fault for address: " << (ctx.vpn << BASE_PAGE_SHIFT) << std::endl;
#endif

    int core_id = this->m_core->getId();

    // 1. Allocate the data frame via the revelator allocator
    assert(this->getAllocator() != NULL);
    const auto &[ppn, page_size] = this->getAllocator()->allocate(FOUR_KIB, ctx.vpn << BASE_PAGE_SHIFT, core_id, false, ctx.alloc_in.is_instruction);

#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    rev_log_file << "[REVELATOR_EXCEPTION_HANDLER] Allocated data page: " << ppn << " with page size: " << page_size << std::endl;
    std::cout << "[REVELATOR_EXCEPTION_HANDLER] Allocated data page: " << ppn << " with page size: " << page_size << std::endl;
#endif

    // Populate output parameters
    ctx.alloc_out.ppn = ppn;
    ctx.alloc_out.page_size = page_size;

    // 2. Allocate page table frames using Revelator-specific logic
    allocate_page_table_frames_revelator(ctx, ctx.vpn << BASE_PAGE_SHIFT, core_id, ppn, page_size, ctx.alloc_in.metadata_frames);

#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    rev_log_file << "[REVELATOR_EXCEPTION_HANDLER] Page table frames allocated for address: " << (ctx.vpn << BASE_PAGE_SHIFT)
                 << " -- allocated " << ctx.alloc_out.prealloc_frames.size() << " frames" << std::endl;
    std::cout << "[REVELATOR_EXCEPTION_HANDLER] Page table frames allocated for address: " << (ctx.vpn << BASE_PAGE_SHIFT)
              << " -- allocated " << ctx.alloc_out.prealloc_frames.size() << " frames" << std::endl;
#endif

    return;
}

void RevelatorExceptionHandler::allocate_page_table_frames_revelator(FaultCtx &ctx,
    UInt64 address, UInt64 core_id, UInt64 ppn, int page_size, int num_requested_frames)
{
    // Revelator allocates PT frames via allocate() with is_pagetable_allocation=true,
    // NOT via handle_page_table_allocations().

#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    rev_log_file << "[REVELATOR_EXCEPTION_HANDLER] Allocating " << num_requested_frames << " page table frames" << std::endl;
    std::cout << "[REVELATOR_EXCEPTION_HANDLER] Allocating " << num_requested_frames << " page table frames" << std::endl;
#endif

    int page_table_frames = num_requested_frames;
    std::vector<UInt64> &frames = ctx.alloc_out.prealloc_frames;

    // Revelator key insight: the LEAF-level PT frame must be placed at hash(address >> 21)
    // so that the spec engine can predict co-located data and PT entries.
    // updatePageTableFrames consumes frames top-down: frames[0] = highest missing level,
    // frames[last] = leaf level. So we allocate non-leaf frames first (generic path),
    // then the leaf frame last (via revelator hash path).

    // 1. Allocate non-leaf frames (levels above the leaf) via generic kernel allocator
    for (int i = 0; i < page_table_frames - 1; i++)
    {
        UInt64 frame = this->getAllocator()->handle_page_table_allocations(FOUR_KIB, core_id);

#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
        rev_log_file << "[REVELATOR_EXCEPTION_HANDLER] Non-leaf PT frame: " << frame << std::endl;
        std::cout << "[REVELATOR_EXCEPTION_HANDLER] Non-leaf PT frame: " << frame << std::endl;
#endif

        if (frame == static_cast<UInt64>(-1))
        {
            std::cerr << "[FATAL] [REVELATOR_EXCEPTION_HANDLER] Out of memory while allocating non-leaf page table frames" << std::endl;
            assert(false);
        }

        frames.push_back(frame);
    }

    // 2. Allocate the LEAF frame via revelator's hash-based allocator (address >> 21)
    {
        auto result = this->getAllocator()->allocate(FOUR_KIB, address, core_id, true);
        UInt64 leaf_frame = result.first;

#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
        rev_log_file << "[REVELATOR_EXCEPTION_HANDLER] Leaf PT frame (hashed): " << leaf_frame << std::endl;
        std::cout << "[REVELATOR_EXCEPTION_HANDLER] Leaf PT frame (hashed): " << leaf_frame << std::endl;
#endif

        if (leaf_frame == static_cast<UInt64>(-1))
        {
            std::cerr << "[FATAL] [REVELATOR_EXCEPTION_HANDLER] Out of memory while allocating leaf page table frame" << std::endl;
            assert(false);
        }

        frames.push_back(leaf_frame);
    }

    // Update page table with allocated frames
    MimicOS *os = Sim()->getMimicOS();
    assert(os);
    Core *core_faulter = Sim()->getCoreManager()->getCoreFromID(core_id);
    assert(core_faulter);
    Thread *thread_faulter = core_faulter->getThread();
    assert(thread_faulter);
    int app_id_faulter = thread_faulter->getAppId();

    assert(os->getPageTable(app_id_faulter));
    int frames_used = os->getPageTable(app_id_faulter)->updatePageTableFrames(address, core_id, ppn, page_size, frames);

#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    rev_log_file << "[REVELATOR_EXCEPTION_HANDLER] Frames requested: " << page_table_frames
                 << ", frames used: " << frames_used << std::endl;
    std::cout << "[REVELATOR_EXCEPTION_HANDLER] Frames requested: " << page_table_frames
              << ", frames used: " << frames_used << std::endl;
#endif

    // Deallocate unused frames with the correct method per frame type:
    // - frames[0..page_table_frames-2] are non-leaf (allocated via handle_page_table_allocations)
    // - frames[page_table_frames-1] is the leaf (allocated via revelator allocate)
    int non_leaf_count = page_table_frames - 1;
    for (int i = frames_used; i < page_table_frames; i++)
    {
        if (i < non_leaf_count)
        {
            // Non-leaf frame: deallocate via generic path
#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
            rev_log_file << "[REVELATOR_EXCEPTION_HANDLER] Deallocating unused non-leaf frame" << std::endl;
#endif
            this->getAllocator()->handle_page_table_deallocations(FOUR_KIB);
        }
        else
        {
            // Leaf frame: deallocate via revelator path
#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
            rev_log_file << "[REVELATOR_EXCEPTION_HANDLER] Deallocating unused leaf frame: " << frames[i] << std::endl;
#endif
            this->getAllocator()->deallocate(frames[i], core_id);
        }
    }

    return;
}
