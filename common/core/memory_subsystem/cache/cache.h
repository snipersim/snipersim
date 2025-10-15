#ifndef CACHE_H
#define CACHE_H

#include "cache_base.h"
#include "cache_set.h"
#include "cache_block_info.h"
#include "utils.h"
#include "hash_map_set.h"
#include "cache_perf_model.h"
#include "shmem_perf_model.h"
#include "log.h"
#include "core.h"
#include "fault_injection.h"

#include <vector>
#include <utility>
#include <cstdlib>

// Define to enable the set usage histogram
//#define ENABLE_SET_USAGE_HIST

class Cache : public CacheBase
{
   private:
      bool m_enabled;

      // Cache counters
      UInt64 m_num_accesses;
      UInt64 m_num_hits;

      // Generic Cache Info
      cache_t m_cache_type;
      CacheSet** m_sets;
      CacheSetInfo* m_set_info;

      FaultInjector *m_fault_injector;

      // --- Hybrid insert masks (optional) ---
      // When enabled, restricts which ways can be chosen on insert.
      // By default, data inserts use all ways (0xFFFFFFFFu) and tag-only inserts use none (0u).
      bool   m_use_hybrid_insert_masks;
      UInt32 m_hybrid_allowed_way_mask_data;  // For inserts that carry data (fill_buff != NULL)
      UInt32 m_hybrid_allowed_way_mask_tag;   // For tag-only inserts (fill_buff == NULL)

      String m_cfgname;
      bool m_hybrid_enabled;
      bool m_hybrid_fill_to_mram;
      UInt32 m_sram_way_mask;
      UInt32 m_mram_way_mask;

      // ---- Per-line tech mapping (optional) ----
      // Two lightweight options:
      //  (a) set-parity: even sets -> one tech, odd sets -> the other
      //  (b) address ranges: [start,end) -> tech
      struct RangeRule {
         IntPtr start;      // inclusive
         IntPtr end;        // exclusive
         bool   to_mram;    // true => MRAM, false => SRAM
      };
      bool m_map_use_set_parity = false;
      bool m_map_even_sets_are_mram = false;
      std::vector<RangeRule> m_addr_rules;

      // Returns true if a per-line decision exists; writes desired tech in *to_mram.
      bool decideLineTech(IntPtr addr, UInt32 set_index, bool *to_mram) const;

      #ifdef ENABLE_SET_USAGE_HIST
      UInt64* m_set_usage_hist;
      #endif

   public:

      // constructors/destructors
      Cache(String name,
            String cfgname,
            core_id_t core_id,
            UInt32 num_sets,
            UInt32 associativity, UInt32 cache_block_size,
            String replacement_policy,
            cache_t cache_type,
            hash_t hash = CacheBase::HASH_MASK,
            FaultInjector *fault_injector = NULL,
            AddressHomeLookup *ahl = NULL);
      ~Cache();

      Lock& getSetLock(IntPtr addr);

      bool invalidateSingleLine(IntPtr addr);
      CacheBlockInfo* accessSingleLine(IntPtr addr,
            access_t access_type, Byte* buff, UInt32 bytes, SubsecondTime now, bool update_replacement);
      void insertSingleLine(IntPtr addr, Byte* fill_buff,
            bool* eviction, IntPtr* evict_addr,
            CacheBlockInfo* evict_block_info, Byte* evict_buff, SubsecondTime now, CacheCntlr *cntlr = NULL);
      CacheBlockInfo* peekSingleLine(IntPtr addr);

      CacheBlockInfo* peekBlock(UInt32 set_index, UInt32 way) const { return m_sets[set_index]->peekBlock(way); }

      // Update Cache Counters
      void updateCounters(bool cache_hit);
      void updateHits(Core::mem_op_t mem_op_type, UInt64 hits);

      void enable() { m_enabled = true; }
      void disable() { m_enabled = false; }

      // ---- Hybrid mask configuration ----
      // Enable hybrid behavior with explicit masks.
      // Any bit i set to 1 allows choosing way i during insertion.
      void setHybridInsertMasks(UInt32 data_way_mask, UInt32 tag_only_way_mask)
      {
         m_use_hybrid_insert_masks = true;
         m_hybrid_allowed_way_mask_data = data_way_mask;
         m_hybrid_allowed_way_mask_tag  = tag_only_way_mask;
      }

      // Convenience setters if only one mask changes.
      void setHybridDataInsertMask(UInt32 data_way_mask)
      {
         m_use_hybrid_insert_masks = true;
         m_hybrid_allowed_way_mask_data = data_way_mask;
      }

      void setHybridTagOnlyInsertMask(UInt32 tag_only_way_mask)
      {
         m_use_hybrid_insert_masks = true;
         m_hybrid_allowed_way_mask_tag = tag_only_way_mask;
      }

      // Disable hybrid behavior (revert to unrestricted inserts).
      void clearHybridInsertMasks()
      {
         m_use_hybrid_insert_masks = false;
         m_hybrid_allowed_way_mask_data = 0xFFFFFFFFu;
         m_hybrid_allowed_way_mask_tag  = 0u;
      }
};

template <class T>
UInt32 moduloHashFn(T key, UInt32 hash_fn_param, UInt32 num_buckets)
{
   return (key >> hash_fn_param) % num_buckets;
}

#endif /* CACHE_H */
