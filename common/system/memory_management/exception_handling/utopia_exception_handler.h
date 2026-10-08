#ifndef UTOPIA_EXCEPTION_HANDLER_H
#define UTOPIA_EXCEPTION_HANDLER_H

#include "sniper_space_exception_handler.h"

/**
 * @brief Page fault handler for the Utopia allocator.
 *
 * The Utopia allocator places a page either in a RestSeg -- where its
 * physical address follows from (set, way) and the Utopia MMU's RestSeg
 * walker translates it with no page table involved -- or in the FlexSeg,
 * where it needs page table entries like any other page.  This handler
 * allocates through the allocator's hierarchy and then updates the page
 * table only for FlexSeg pages; a RestSeg page gets no page table frames
 * and no entry.
 *
 * Under an MMU without a RestSeg walker (type other than utopia), a page
 * that the page table does not map could never be translated, so the
 * handler then maps RestSeg pages too: the page table stays authoritative
 * for every page and only the placement is Utopia's.
 *
 * Derives from SniperExceptionHandler, as the Revelator handler does, so the
 * walk-restart sites that cast to it keep working, and so the page table
 * frame allocation and the guest/host selection are inherited.
 */
class UtopiaExceptionHandler : public SniperExceptionHandler {
public:
    UtopiaExceptionHandler(Core* core);
    ~UtopiaExceptionHandler();

    void handle_page_fault(FaultCtx& ctx) override;

private:
    bool m_map_restseg_in_page_table;   // true under an MMU without a RestSeg walker
    UInt64 m_restseg_faults;            // faults whose page went to a RestSeg (no page table update)
    UInt64 m_flexseg_faults;            // faults whose page went to the FlexSeg
};

#endif // UTOPIA_EXCEPTION_HANDLER_H
