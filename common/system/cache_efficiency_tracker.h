#ifndef __CACHE_EFFICIENCY_TRACKER_H
#define __CACHE_EFFICIENCY_TRACKER_H

#include "cache_block_info.h"

namespace CacheEfficiencyTracker
{
   typedef UInt64 (*CallbackGetOwner)(UInt64 user, core_id_t core_id);
   typedef void (*CallbackNotify)(UInt64 user, bool on_roi_end, UInt64 owner, CacheBlockInfo::BitsUsedType bits_used, UInt32 bits_total);

   struct Callbacks
   {
      CacheEfficiencyTracker::CallbackGetOwner get_owner_func;
      CacheEfficiencyTracker::CallbackNotify notify_func;
      UInt64 user_arg;

      Callbacks() : get_owner_func(NULL), notify_func(NULL), user_arg(0) {}

      UInt64 call_get_owner(core_id_t core_id) const { return get_owner_func(user_arg, core_id); }
      void call_notify(bool on_roi_end, UInt64 owner, CacheBlockInfo::BitsUsedType bits_used, UInt32 bits_total) const
      { notify_func(user_arg, on_roi_end, owner, bits_used, bits_total); }
   };
};

#endif // __CACHE_EFFICIENCY_TRACKER_H
