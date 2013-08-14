#ifndef __TOPOLOGY_INFO_H
#define __TOPOLOGY_INFO_H

#include "fixed_types.h"

class TopologyInfo
{
   public:
      TopologyInfo(core_id_t _core_id)
         : core_id(_core_id)
         , smt_index(-1)
         , smt_count(-1)
         , core_index(-1)
         , core_count(-1)
         , package(-1)
      {}
      void setup(UInt32 smt_cores, UInt32 llc_sharers);

      core_id_t core_id;
      SInt32 smt_index, smt_count;
      SInt32 core_index, core_count;
      SInt32 package;

      static SInt32 s_package, s_cores_this_package, s_core_id_last;
};

#endif // __TOPOLOGY_INFO_H
