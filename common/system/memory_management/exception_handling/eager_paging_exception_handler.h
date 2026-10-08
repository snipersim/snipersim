#ifndef EAGER_PAGING_EXCEPTION_HANDLER_H
#define EAGER_PAGING_EXCEPTION_HANDLER_H

#include "misc/exception_handler_base.h"
#include "sim_log.h"
#include <vector>

class MimicOS;
class VMA;

/**
 * @brief The page-fault handler of Redundant Memory Mappings' eager paging
 *
 * The first fault in a VMA lays the whole VMA out: the allocator hands back
 * its physically contiguous ranges, every page of every range is mapped in
 * the page table, and each range goes into the range table the range MMU
 * translates from.  A fault outside every VMA (a region mapped after the
 * trace's VMA log was taken) is served one page at a time, as demand paging
 * would.  VMAs come only from the trace's .vma file; a run without any
 * cannot page eagerly at all and stops with a message saying so.
 */
class EagerPagingExceptionHandler : public ExceptionHandlerBase {
public:
    EagerPagingExceptionHandler(Core* core);
    ~EagerPagingExceptionHandler();

    void handle_exception(int exception_type_code, int argc, uint64_t *argv) override;
    void handle_page_fault(FaultCtx& ctx) override;
    PhysicalMemoryAllocator* getAllocator() override;

private:
    SimLog *m_log;
    struct
    {
        UInt64 vma_faults;        // faults that laid a VMA out
        UInt64 pages_mapped;      // pages mapped by those
        UInt64 no_vma_faults;     // faults outside every VMA, served one page at a time
    } m_stats;

    void allocate_vma_eagerly(FaultCtx& ctx, VMA& vma, int app_id);
    void map_page(FaultCtx& ctx, UInt64 address, UInt64 ppn, int page_size);
};

#endif // EAGER_PAGING_EXCEPTION_HANDLER_H
