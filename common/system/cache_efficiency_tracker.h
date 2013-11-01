#ifndef __CACHE_EFFICIENCY_TRACKER_H
#define __CACHE_EFFICIENCY_TRACKER_H

#include "cache_block_info.h"
#include "hit_where.h"
#include "core.h"

namespace CacheEfficiencyTracker
{
   typedef UInt64 (*CallbackGetOwner)(UInt64 user, core_id_t core_id, UInt64 address);
   typedef void (*CallbackNotifyAccess)(UInt64 user, UInt64 owner, Core::mem_op_t mem_op_type, HitWhere::where_t hit_where);
   typedef void (*CallbackNotifyEvict)(UInt64 user, bool on_roi_end, UInt64 owner, UInt64 evictor, CacheBlockInfo::BitsUsedType bits_used, UInt32 bits_total);

   struct Callbacks
   {
      CacheEfficiencyTracker::CallbackGetOwner get_owner_func;
      CacheEfficiencyTracker::CallbackNotifyAccess notify_access_func;
      CacheEfficiencyTracker::CallbackNotifyEvict notify_evict_func;
      UInt64 user_arg;

      Callbacks() : get_owner_func(NULL), notify_access_func(NULL), notify_evict_func(NULL), user_arg(0) {}

      UInt64 call_get_owner(core_id_t core_id, UInt64 address) const { return get_owner_func(user_arg, core_id, address); }
      void call_notify_access(UInt64 owner, Core::mem_op_t mem_op_type, HitWhere::where_t hit_where) const
      { notify_access_func(user_arg, owner, mem_op_type, hit_where); }
      void call_notify_evict(bool on_roi_end, UInt64 owner, UInt64 evictor, CacheBlockInfo::BitsUsedType bits_used, UInt32 bits_total) const
      { notify_evict_func(user_arg, on_roi_end, owner, evictor, bits_used, bits_total); }
   };
};

#endif // __CACHE_EFFICIENCY_TRACKER_H
