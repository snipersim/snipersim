#ifndef __DRAM_CNTLR_INTERFACE_H
#define __DRAM_CNTLR_INTERFACE_H

#include "fixed_types.h"
#include "subsecond_time.h"

class DramCntlrInterface
{
   public:
      virtual ~DramCntlrInterface() {}

      virtual SubsecondTime getDataFromDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now) = 0;
      virtual SubsecondTime putDataToDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now) = 0;
};

#endif // __DRAM_CNTLR_INTERFACE_H
