#ifndef SNIPER_SPACE_EXCEPTION_HANDLER_H
#define SNIPER_SPACE_EXCEPTION_HANDLER_H

#include "misc/exception_handler_base.h"
#include <fstream>

class MimicOS;

class SniperExceptionHandler : public ExceptionHandlerBase {
private:
    std::ofstream log_file;
public:
    SniperExceptionHandler(Core* core);
    ~SniperExceptionHandler();

    void handle_exception(int exception_type_code, int argc, uint64_t *argv) override;
    void handle_page_fault(FaultCtx& ctx) override;
    PhysicalMemoryAllocator* getAllocator() override; 

    ExceptionHandlerBase::FaultCtx initFaultCtx(ParametricDramDirectoryMSI::PageTable* page_table, UInt64 address, UInt64 core_id, int requested_frames, bool is_instruction = false) {
            ExceptionHandlerBase::FaultCtx fault_ctx{};
            fault_ctx.vpn = address >> 12;
            fault_ctx.page_table = page_table;
            fault_ctx.alloc_in.metadata_frames = requested_frames;
            fault_ctx.alloc_in.is_instruction = is_instruction;
            return fault_ctx;
    }

protected:
    // Helper functions, for the handlers that derive from this one as well
    // The OS a fault belongs to: the guest OS when the faulting page table is
    // a guest one (the virt MMU raises those), the host OS otherwise.
    MimicOS* osFor(const FaultCtx& ctx);
    PhysicalMemoryAllocator* allocatorFor(const FaultCtx& ctx);
    void allocate_page_table_frames(FaultCtx& ctx,
                                    UInt64 address, UInt64 core_id, UInt64 ppn, int page_size, int num_requested_frames);
    int update_page_table_frames(FaultCtx& ctx, UInt64 address, UInt64 core_id, UInt64 ppn, int page_size, std::vector<UInt64> &frames);
};

#endif // SNIPER_SPACE_EXCEPTION_HANDLER_H
