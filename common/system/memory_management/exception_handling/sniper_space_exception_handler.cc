
#include "sniper_space_exception_handler.h"
#include "core.h"
#include "simulator.h"
#include "mimicos.h"
#include "memory_management/physical_memory_allocators/physical_memory_allocator.h"
#include "thread.h"
#include "pagetable.h"
#include "debug_config.h"
#include "hugetlbfs/hugetlbfs.h"

// Static lock definition for thread-safe page fault handling across all cores
// SpinLock: non-fair, avoids FIFO ordering overhead of ticket lock
SpinLock ExceptionHandlerBase::s_page_fault_lock;

SniperExceptionHandler::SniperExceptionHandler(Core *core) : ExceptionHandlerBase(core)
{
    // Initialise log structures
    std::cout << std::endl;
    std::cout << "[EXCEPTION_HANDLER] Initializing Exception Handler (sniper-space MimicOS)" << std::endl;
    log_file = std::ofstream();
    std::string log_file_name = "exception_handler.log." + std::to_string(core->getId());
    log_file_name = std::string(Sim()->getConfig()->getOutputDirectory().c_str()) + "/" + log_file_name;
    log_file.open(log_file_name.c_str());
    log_file << "[EXCEPTION_HANDLER] Initialised from the Core constructor" << std::endl;
    std::cout << "[EXCEPTION_HANDLER] Initialised from the Core constructor" << std::endl;
}

SniperExceptionHandler::~SniperExceptionHandler()
{
    if (log_file.is_open())
    {
        log_file.close();
    }
    std::cout << "[EXCEPTION_HANDLER] SniperExceptionHandler destructor called" << std::endl;
}

void SniperExceptionHandler::handle_exception(int exception_type_code, int argc, uint64_t *argv)
{
    // Handle the exception
    std::cout << "[SniperExceptionHandler] handle_exception not implemented yet... exiting with status code 1" << std::endl;
    exit(1);
}

void SniperExceptionHandler::handle_page_fault(FaultCtx &ctx)
{
    // Acquire global lock for thread-safe page fault handling in multi-core
    ScopedLock sl(ExceptionHandlerBase::s_page_fault_lock);

#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    std::cout << "[EXCEPTION_HANDLER] Handling page fault for address: " << (ctx.vpn << BASE_PAGE_SHIFT) << " in Sniper-space" << std::endl;
#endif
    int core_id = this->m_core->getId();

    // TODO migrate swapping...
    // bool is_swapped = false;
    // if(Sim()->getMimicOS()->isSwapEnabled()){
    //     // If the swap is enabled, we need to check if the page is in the swap space
    //     is_swapped = handleSwapping(address, core_id);
    // }
    // if(is_swapped)
    //     Sim()->getMimicOS()->setLastPageFaultCausedSwapping(is_swapped);

    // 1. Allocate the frames
#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    log_file << "[EXCEPTION_HANDLER] Handling page fault for address: " << (ctx.vpn << BASE_PAGE_SHIFT) << " for core: " << core_id << std::endl;
    log_file << "[EXCEPTION_HANLDER] We need to allocate: " << (ctx.alloc_in.metadata_frames) << " frames" << std::endl;
#endif

    // A guest page table faults into guest-physical memory, a host one into
    // host-physical memory; the allocator follows the page table in ctx.
    PhysicalMemoryAllocator *allocator = this->allocatorFor(ctx);
    assert(allocator != NULL);
    MimicOS *os = this->osFor(ctx);
    assert(os);

    UInt64 ppn = static_cast<UInt64>(-1);
    UInt64 page_size = 0;
    // The address the mapping is installed at: the faulting page, or the
    // whole 2 MB block when a huge page backs it.
    UInt64 map_address = ctx.vpn << BASE_PAGE_SHIFT;

    // 1a. The hugetlbfs pool, when it backs this block of the faulting
    // application's address space (MimicOS::hugePageBlockFor: the service
    // enabled, the block inside a VMA its criterion accepts).  The pool hands
    // out a 2 MB frame whose base ppn is in 4 KB units -- the same shape the
    // reserve_thp allocator returns for a promoted region -- and the mapping
    // is installed at the block's base with page_size 21, so the radix table
    // writes one level-2 PTE and the walk finds a 2 MB translation.  An empty
    // pool is counted by the service (allocation_failures,
    // pool_exhausted_events) and the fault falls through to the allocator.
    if (os->isHugeTLBfsEnabled())
    {
        Core *core_faulter = Sim()->getCoreManager()->getCoreFromID(core_id);
        Thread *thread_faulter = core_faulter ? core_faulter->getThread() : NULL;
        if (thread_faulter)
        {
            int app_id = thread_faulter->getAppId();
            IntPtr block = os->hugePageBlockFor(app_id, map_address);
            if (block != static_cast<IntPtr>(-1))
            {
                IntPtr huge_ppn = os->allocateHugePage(app_id, block, true);
                if (huge_ppn != static_cast<IntPtr>(-1))
                {
                    ppn = huge_ppn;
                    page_size = HUGEPAGE_2MB_BITS;
                    map_address = block;
#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
                    log_file << "[EXCEPTION_HANDLER] hugetlbfs 2MB page for block 0x" << std::hex << block
                             << std::dec << ": ppn " << ppn << std::endl;
#endif
                }
#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
                else
                    log_file << "[EXCEPTION_HANDLER] hugetlbfs pool exhausted for block 0x" << std::hex << block
                             << std::dec << ", falling back to the allocator" << std::endl;
#endif
            }
        }
    }

    // 1b. The physical memory allocator.
    if (ppn == static_cast<UInt64>(-1))
    {
        const auto &[alloc_ppn, alloc_page_size] = allocator->allocate(FOUR_KIB, map_address, core_id, false, ctx.alloc_in.is_instruction);
        ppn = alloc_ppn;
        page_size = alloc_page_size;
    }

#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    log_file << "[EXCEPTION_HANDLER] Allocated page: " << ppn << " with page size: " << page_size << std::endl;
    std::cout << "[EXCEPTION_HANDLER] Allocated page: " << ppn << " with page size: " << page_size << std::endl;
#endif
    // Populate output paramaters
    ctx.alloc_out.ppn = ppn;
    ctx.alloc_out.page_size = page_size;

    // 2. Update PTs
    allocate_page_table_frames(ctx, map_address, core_id, ppn, page_size, ctx.alloc_in.metadata_frames);
#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    log_file << "[EXCEPTION_HANDLER] Page table frames allocated for address: " << (ctx.vpn << BASE_PAGE_SHIFT) <<
                " -- allocated " << ctx.alloc_out.prealloc_frames.size() << " frames" << std::endl;
    std::cout << "[EXCEPTION_HANDLER] Page table frames allocated for address: " << (ctx.vpn << BASE_PAGE_SHIFT) <<
                " -- allocated " << ctx.alloc_out.prealloc_frames.size() << " frames" << std::endl;
#endif

    // If the page is allocated, return
    return;
}

void SniperExceptionHandler::allocate_page_table_frames(FaultCtx &ctx,
                                                        UInt64 address, UInt64 core_id, UInt64 ppn, int page_size, int num_requested_frames)
{
    // First lets ask the page table if new frames are needed
#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    log_file << "[EXCEPTION_HANDLER] Allocating page table frames: " << num_requested_frames << std::endl;
    std::cout << "[EXCEPTION_HANDLER] Allocating page table frames: " << num_requested_frames << std::endl;
#endif

    int page_table_frames = num_requested_frames;
    std::vector<UInt64> &frames = ctx.alloc_out.prealloc_frames;
    PhysicalMemoryAllocator *allocator = this->allocatorFor(ctx);

    // TODO: migrate other allocators - allocate metadata frames stage
    for (int i = 0; i < page_table_frames; i++)
    {
        UInt64 frame = allocator->handle_page_table_allocations(FOUR_KIB, core_id);
#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
        log_file << "[EXCEPTION_HANDLER] Giving away page table frame: " << frame << std::endl;
        std::cout << "[EXCEPTION_HANDLER] Giving away page table frame: " << frame << std::endl;
#endif
        if (frame == static_cast<UInt64>(-1))
        {
            // We are out of memory
            std::cerr << "[FATAL] [EXCEPTION_HANDLER] Out of memory while allocating page table frames" << std::endl;
            std::cerr << "[FATAL] [EXCEPTION_HANDLER] Exiting with status code 1..." << std::endl;
            assert(false);
        }

        frames.push_back(frame);
    }

    // Virtualisation: the page table to update is the one named in ctx, which
    // for a guest fault is the guest page table (see update_page_table_frames).

    // create a function update_page_table_frames that will update the page table frames
    int frames_used = update_page_table_frames(ctx, address, core_id, ppn, page_size, frames);

#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    log_file << "[EXCEPTION_HANDLER] Page table frames used: " << frames_used << std::endl;
    std::cout<< "[EXCEPTION_HANDLER] Page table frames used: " << frames_used << std::endl;
#endif

    // TODO: migrate other allocators - deallocation of unused frames stage
#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    log_file << "[EXCEPTION_HANDLER] Frames requested: " << page_table_frames << std::endl;
    std::cout << "[EXCEPTION_HANDLER] Frames requested: " << page_table_frames << std::endl;
    log_file << "[EXCEPTION_HANDLER] Frames used: " << frames_used << std::endl;
    std::cout << "[EXCEPTION_HANDLER] Frames used: " << frames_used << std::endl;
    log_file << "[EXCEPTION_HANDLER] Deallocating page table frames: " << (page_table_frames - frames_used) << std::endl;
    std::cout << "[EXCEPTION_HANDLER] Deallocating page table frames: " << (page_table_frames - frames_used) << std::endl;
#endif
    for (int i = 0; i < (page_table_frames - frames_used); i++)
    {
        allocator->handle_page_table_deallocations(FOUR_KIB);
    }
    return;
}

int SniperExceptionHandler::update_page_table_frames(FaultCtx &ctx, UInt64 address, UInt64 core_id, UInt64 ppn, int page_size, std::vector<UInt64> &frames)
{

#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    log_file << "[EXCEPTION_HANDLER] Updating page table frames for address: " << address << " with ppn: " << ppn << " and page size: " << page_size << std::endl;
    std::cout << "[EXCEPTION_HANDLER] Updating page table frames for address: " << address << " with ppn: " << ppn << " and page size: " << page_size << std::endl;
#endif
    MimicOS *os = this->osFor(ctx);
    assert(os);
    Core *core_faulter = Sim()->getCoreManager()->getCoreFromID(core_id);
    assert(core_faulter);
    Thread *thread_faulter = core_faulter->getThread();
    assert(thread_faulter);
    int app_id_faulter = thread_faulter->getAppId();

#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    log_file << "[EXCEPTION_HANDLER] App ID of the faulter: " << app_id_faulter << std::endl;
    std::cout << "[EXCEPTION_HANDLER] App ID of the faulter: " << app_id_faulter << std::endl;
#endif

    // Update the page table frames
#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    log_file << "[EXCEPTION_HANDLER] Updating page table frames for address: " << address << " with ppn: " << ppn << " and page size: " << page_size << std::endl;
    std::cout << "[EXCEPTION_HANDLER] Updating page table frames for address: " << address << " with ppn: " << ppn << " and page size: " << page_size << std::endl;
#endif
    // The page table that faulted is the one to map into.  It is the faulting
    // application's page table in its OS; for a guest fault raised by the virt
    // MMU that OS is the guest's, where the host-only lookup used to land the
    // mapping in the host page table and leave the guest walk faulting forever.
    ParametricDramDirectoryMSI::PageTable *page_table = ctx.page_table ? ctx.page_table : os->getPageTable(app_id_faulter);
    assert(page_table);
    int frames_used = page_table->updatePageTableFrames(address, core_id, ppn, page_size, frames);
#if DEBUG_EXCEPTION_HANDLER >= DEBUG_BASIC
    log_file << "[EXCEPTION_HANDLER] Frames used: " << frames_used << std::endl;
    std::cout << "[EXCEPTION_HANDLER] Frames used: " << frames_used << std::endl;
#endif
    // TODO: migrate deallocation allocators as well
    return frames_used;
}


MimicOS* SniperExceptionHandler::osFor(const FaultCtx &ctx)
{
    if (ctx.page_table != NULL && ctx.page_table->isGuest())
        return Sim()->getMimicOS_VM();
    return Sim()->getMimicOS();
}

PhysicalMemoryAllocator* SniperExceptionHandler::allocatorFor(const FaultCtx &ctx)
{
    if (ctx.page_table != NULL && ctx.page_table->isGuest())
        return Sim()->getMimicOS_VM()->getMemoryAllocator();
    return this->getAllocator();
}

PhysicalMemoryAllocator* SniperExceptionHandler::getAllocator() { 
    if (m_allocator == NULL) {
        std::cout << "[EXCEPTION_HANDLER] Allocator is NULL, getting it from the MimicOS" << std::endl;
        m_allocator = Sim()->getMimicOS()->getMemoryAllocator();
    }

    return m_allocator; 
}