#ifndef __DRAM_CNTLR_INTERFACE_H
#define __DRAM_CNTLR_INTERFACE_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include "hit_where.h"

#include "boost/tuple/tuple.hpp"

class DramCntlrInterface
{
   public:
      virtual ~DramCntlrInterface() {}

      virtual boost::tuple<SubsecondTime, HitWhere::where_t> getDataFromDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now) = 0;
      virtual boost::tuple<SubsecondTime, HitWhere::where_t> putDataToDram(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now) = 0;
};

#endif // __DRAM_CNTLR_INTERFACE_H
