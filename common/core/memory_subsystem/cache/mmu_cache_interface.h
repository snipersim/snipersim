#pragma once

#include "fixed_types.h"
#include "hit_where.h"
#include "subsecond_time.h"
#include "core.h"
#include "cache_block_info.h"

/**
 * @brief Abstract interface for cache controllers accessed by the MMU subsystem.
 *
 * This interface decouples the MMU from any specific cache controller implementation,
 * allowing both the parametric (coherent) and fast_detailed (non-coherent) memory
 * managers to provide cache access for page table walks.
 */
class MMUCacheInterface
{
public:
   /**
    * @brief Process a memory operation from the MMU (page table walk cache access).
    *
    * @return HitWhere::where_t indicating where the access hit (L1, L2, LLC, DRAM, etc.)
    */
   virtual HitWhere::where_t handleMMUCacheAccess(
       IntPtr eip,
       Core::lock_signal_t lock_signal,
       Core::mem_op_t mem_op_type,
       IntPtr ca_address, UInt32 offset,
       Byte *data_buf, UInt32 data_length,
       bool modeled, bool count,
       CacheBlockInfo::block_type_t block_type,
       SubsecondTime t_start) = 0;

   /**
    * @brief Issue a prefetch from the MMU.
    */
   virtual void handleMMUPrefetch(
       IntPtr eip, IntPtr prefetch_address,
       SubsecondTime t_start,
       CacheBlockInfo::block_type_t block_type = CacheBlockInfo::block_type_t::DATA)
   {
      // Default: no-op (prefetching not supported in all memory managers)
   }

   /**
    * @brief Return the completion time of the most recent prefetch.
    *
    * Used by speculative engines to compute timing deltas.
    * Default returns Zero for implementations that don't track prefetch timing.
    */
   virtual SubsecondTime getLastPrefetchCompletion() const
   {
      return SubsecondTime::Zero();
   }

   /**
    * @brief Tag a resident line as prefetched (CacheBlockInfo::PREFETCH).
    *
    * Called once a prefetch page-table walk has filled a line through the
    * normal demand path, so that a later demand hit on it is credited as a
    * prefetch hit rather than an ordinary one.
    */
   virtual void tagMMUPrefetch(IntPtr cache_address,
                               HitWhere::where_t hit_where = HitWhere::UNKNOWN)
   {
      // Default: no-op
   }

   /**
    * @brief Mark an MMU-prefetched line for per-PTE (sub-line) credit.
    *
    * A cache line holds eight PTEs, and the single PREFETCH bit can only be
    * spent once.  This marks each 8-byte sub-region separately so that every
    * distinct PTE a later demand walk consumes is counted, which is what makes
    * a prefetch that supplied several PTEs distinguishable from one that
    * supplied a single PTE.
    */
   virtual void markMMUPrefetchSubline(IntPtr cache_address,
                                       HitWhere::where_t hit_where = HitWhere::UNKNOWN)
   {
      // Default: no-op
   }

   /**
    * @brief Is this line resident in *this* level?  Tag lookup only.
    *
    * No fill, no replacement update, no latency charged.  Used by speculative
    * parallel-probe paths.  False for backends that cannot peek.
    */
   virtual bool isLinePresent(IntPtr cache_address)
   {
      return false;
   }

   /**
    * @brief Mark a resident line dirty without modelling a store.
    *
    * Models the writeback traffic caused by updating a prefetch payload stored
    * inside a PTE: the PTE's line becomes dirty and will be written back when
    * evicted.  Dirty-if-present -- a miss does not fetch.
    *
    * @return 0 the line is not resident, so no writeback is modelled;
    *         1 a resident clean line was upgraded to dirty;
    *         2 the line was already dirty, so the write coalesces.
    */
   virtual int markMMUPayloadDirty(IntPtr /*cache_address*/)
   {
      return 0;
   }

   virtual ~MMUCacheInterface() = default;
};
