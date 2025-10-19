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
#include "stats.h"
#include <memory>
// @kanellok Define to enable the set usage histogram
// #define ENABLE_SET_USAGE_HIST

class Cache : public CacheBase
{
private:
      bool m_enabled;

      // @kanellok Cache counters
      UInt64 m_num_accesses;
      UInt64 m_num_hits;

      // @kanellok Generic Cache Info
      cache_t m_cache_type;
      CacheSet **m_sets;
      CacheSetInfo *m_set_info;

      FaultInjector *m_fault_injector;

	//unique ptr since there might be a bug that alters the page size list
	std::unique_ptr<int[]> m_pagesizes; //@kanellok TLB supported page size vector
	int m_number_of_page_sizes;
	bool m_is_tlb;
      
      int reuse_levels[3];

      float average_data_reuse;
      float average_metadata_reuse;
      float average_tlb_reuse;

      UInt64 sum_data_reuse;
      UInt64 sum_data_utilization;

      UInt64 sum_metadata_reuse;
      UInt64 sum_metadata_utilization;

      UInt64 sum_tlb_reuse;
      UInt64 sum_tlb_utilization;

      UInt64 number_of_data_reuse;
      UInt64 number_of_metadata_reuse;
      UInt64 number_of_tlb_reuse;

      // @kanellok: Metadata passthrough location: 1: L1, 2: L2
      // This determines where metadata is stored in the cache hierarchy
      // For now, we do not allow metadata to be cached only in the NUCA

      int metadata_passthrough_loc;

      /* @kanellok Reuse prediction implementation */
      /* 0, 1-2, 3-5, 5-10, >10 */

      UInt64 data_reuse[5];
      UInt64 metadata_reuse[5];
      UInt64 tlb_reuse[5];

      UInt64 data_util[8];
      UInt64 metadata_util[8];
      UInt64 tlb_util[8];

#ifdef ENABLE_SET_USAGE_HIST
      UInt64 *m_set_usage_hist;
#endif

public:
      // @kanellok stats vectors to store time-series data for cache blocks used for different purposes
      // This is useful for tracking the contents of the cache over time
      std::vector<uint64_t> m_page_walk_cacheblocks;  /* @kanellok timeseries stats */
      std::vector<uint64_t> m_utopia_cacheblocks;     
      std::vector<uint64_t> m_security_cacheblocks;   
      std::vector<uint64_t> m_expressive_cacheblocks; 
      std::vector<uint64_t> m_tlb_cacheblocks;        

      // @kanellok constructors/destructors
      Cache(String name,
            String cfgname,
            core_id_t core_id,
            UInt32 num_sets,
            UInt32 associativity, UInt32 cache_block_size,
            String replacement_policy,
            cache_t cache_type,
            hash_t hash = CacheBase::HASH_MASK,
            FaultInjector *fault_injector = NULL,
            AddressHomeLookup *ahl = NULL,bool is_tlb = false, int *page_size = NULL, int number_of_page_sizes = 0);
            
      ~Cache();

      Lock &getSetLock(IntPtr addr);

      bool invalidateSingleLine(IntPtr addr);
      CacheBlockInfo *accessSingleLine(IntPtr addr, access_t access_type, Byte *buff, UInt32 bytes, SubsecondTime now, bool update_replacement, bool tlb_entry = false, bool is_metadata = false);
	CacheBlockInfo *accessSingleLineTLB(IntPtr addr, access_t access_type, Byte *buff, UInt32 bytes, SubsecondTime now, bool update_replacement);
                                       
      void insertSingleLine(IntPtr addr, Byte *fill_buff,
                            bool *eviction, IntPtr *evict_addr,
                            CacheBlockInfo *evict_block_info, Byte *evict_buff, SubsecondTime now, CacheCntlr *cntlr = NULL, CacheBlockInfo::block_type_t btype = CacheBlockInfo::block_type_t::NON_PAGE_TABLE);
	
                            void insertSingleLineTLB(IntPtr addr, Byte *fill_buff,bool *eviction, IntPtr *evict_addr, CacheBlockInfo *evict_block_info, Byte *evict_buff, SubsecondTime now, CacheCntlr *cntlr = NULL, CacheBlockInfo::block_type_t btype = CacheBlockInfo::block_type_t::NON_PAGE_TABLE, int page_size = 12, IntPtr ppn = 0);

      CacheBlockInfo *peekSingleLine(IntPtr addr);
      CacheBlockInfo *peekBlock(UInt32 set_index, UInt32 way) const { return m_sets[set_index]->peekBlock(way); }
      void updateSetReplacement(IntPtr addr);

      std::vector<uint64_t> getPTWTranslationStats() { return m_page_walk_cacheblocks; }
      std::vector<uint64_t> getUtopiaTranslationStats() { return m_utopia_cacheblocks; }
      std::vector<uint64_t> getSecurityTranslationStats() { return m_security_cacheblocks; }
      std::vector<uint64_t> getExpressiveTranslationStats() { return m_expressive_cacheblocks; }
      std::vector<uint64_t> getTLBStats() { return m_tlb_cacheblocks; }

      // @kanellok Update Cache Counters
      void updateCounters(bool cache_hit);
      void updateHits(Core::mem_op_t mem_op_type, UInt64 hits);

      void enable() { m_enabled = true; }
      void disable() { m_enabled = false; }

      CacheSet *getCacheSet(UInt32 set_index);

      void measureStats();
      void markMetadata(IntPtr address, CacheBlockInfo::block_type_t blocktype);
};

template <class T>
UInt32 moduloHashFn(T key, UInt32 hash_fn_param, UInt32 num_buckets)
{
      return (key >> hash_fn_param) % num_buckets;
}

#endif /* CACHE_H */
