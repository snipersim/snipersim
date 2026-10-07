#ifndef REVELATOR_EXCEPTION_HANDLER_H
#define REVELATOR_EXCEPTION_HANDLER_H

#include "sniper_space_exception_handler.h"

/**
 * @brief Exception handler for the Revelator memory allocator.
 *
 * Revelator uses a hash-based physical memory allocator that requires
 * page table frames to be allocated via the main allocate() path
 * (with is_pagetable_allocation=true) rather than the generic
 * handle_page_table_allocations() path.  Unused frames are returned
 * via deallocate() instead of handle_page_table_deallocations().
 *
 * Inherits from SniperExceptionHandler so that existing dynamic_cast
 * sites (pagetable_radix.cc, mmu_spec.cc) continue to work.
 */
class RevelatorExceptionHandler : public SniperExceptionHandler {
public:
    RevelatorExceptionHandler(Core* core);
    ~RevelatorExceptionHandler();

    void handle_page_fault(FaultCtx& ctx) override;

private:
    void allocate_page_table_frames_revelator(FaultCtx& ctx,
        UInt64 address, UInt64 core_id, UInt64 ppn, int page_size, int num_requested_frames);
    
    std::ofstream rev_log_file;
};

#endif // REVELATOR_EXCEPTION_HANDLER_H
