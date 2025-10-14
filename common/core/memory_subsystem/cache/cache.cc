#include "simulator.h"
#include "cache.h"
#include "log.h"
#include "config.hpp"
#include "shared_cache_block_info.h"
// Cache class
// constructors/destructors
Cache::Cache(
   String name,
   String cfgname,
   core_id_t core_id,
   UInt32 num_sets,
   UInt32 associativity,
   UInt32 cache_block_size,
   String replacement_policy,
   cache_t cache_type,
   hash_t hash,
   FaultInjector *fault_injector,
   AddressHomeLookup *ahl)
:
   CacheBase(name, num_sets, associativity, cache_block_size, hash, ahl),
   m_cfgname(cfgname),
   m_enabled(false),
   m_num_accesses(0),
   m_num_hits(0),
   m_cache_type(cache_type),
   m_fault_injector(fault_injector),
   // Hybrid masks default: disabled; data inserts unrestricted; tag-only inserts disallowed
   m_use_hybrid_insert_masks(false),
   m_hybrid_allowed_way_mask_data(0xFFFFFFFFu),
   m_hybrid_allowed_way_mask_tag(0u),
   // Hybrid (SRAM/MRAM) defaults
   m_hybrid_enabled(false),
   m_sram_way_mask(0u),
   m_mram_way_mask(0u),
   m_hybrid_fill_to_mram(false)
{
   m_set_info = CacheSet::createCacheSetInfo(name, cfgname, core_id, replacement_policy, m_associativity);
   m_sets = new CacheSet*[m_num_sets];
   for (UInt32 i = 0; i < m_num_sets; i++)
   {
      m_sets[i] = CacheSet::createCacheSet(cfgname, core_id, replacement_policy, m_cache_type, m_associativity, m_blocksize, m_set_info);
   }

   #ifdef ENABLE_SET_USAGE_HIST
   m_set_usage_hist = new UInt64[m_num_sets];
   for (UInt32 i = 0; i < m_num_sets; i++)
      m_set_usage_hist[i] = 0;
   #endif

   // --- HYBRID (SRAM/MRAM) optional config ---
   // Only active if keys exist; otherwise stays disabled (default).
   String hyb_base =  cfgname + "/hybrid";

   if (Sim()->getCfg()->hasKey(hyb_base + "/enabled") && Sim()->getCfg()->getBoolDefault(hyb_base + "/enabled", false))
   {
   m_hybrid_enabled = true;

   // sram_ways (accept array or scalar)
   int sram_ways = (int)m_associativity;
   String k_sram_ways = hyb_base + "/sram_ways";
   if (Sim()->getCfg()->hasKey(k_sram_ways, core_id))
      sram_ways = Sim()->getCfg()->getIntArray(k_sram_ways, core_id);
   else if (Sim()->getCfg()->hasKey(k_sram_ways))
      sram_ways = Sim()->getCfg()->getInt(k_sram_ways);

   if (sram_ways < 0) sram_ways = 0;
   if ((UInt32)sram_ways > m_associativity) sram_ways = (int)m_associativity;

   // Contiguous-at-low-index split: ways [0..sram_ways-1] = SRAM; rest = MRAM
   const UInt32 mask_all = (m_associativity >= 32) ? 0xFFFFFFFFu : ((1u << m_associativity) - 1u);
   m_sram_way_mask = (sram_ways >= 32) ? 0xFFFFFFFFu
                                       : (sram_ways > 0 ? ((1u << sram_ways) - 1u) : 0u);
   m_mram_way_mask = mask_all & ~m_sram_way_mask;

   // fill_to (accept array or scalar)
   String fill_to = "sram";
   String k_fill = hyb_base + "/fill_to";
   if (Sim()->getCfg()->hasKey(k_fill, core_id))
      fill_to = Sim()->getCfg()->getStringArray(k_fill, core_id);
   else if (Sim()->getCfg()->hasKey(k_fill))
      fill_to = Sim()->getCfg()->getString(k_fill);

   m_hybrid_fill_to_mram = (fill_to == "mram");
   }

}

Cache::~Cache()
{
   #ifdef ENABLE_SET_USAGE_HIST
   printf("Cache %s set usage:", m_name.c_str());
   for (SInt32 i = 0; i < (SInt32) m_num_sets; i++)
      printf(" %" PRId64, m_set_usage_hist[i]);
   printf("\n");
   delete [] m_set_usage_hist;
   #endif

   if (m_set_info)
      delete m_set_info;

   for (SInt32 i = 0; i < (SInt32) m_num_sets; i++)
      delete m_sets[i];
   delete [] m_sets;
}

Lock&
Cache::getSetLock(IntPtr addr)
{
   IntPtr tag;
   UInt32 set_index;

   splitAddress(addr, tag, set_index);
   assert(set_index < m_num_sets);

   return m_sets[set_index]->getLock();
}

bool
Cache::invalidateSingleLine(IntPtr addr)
{
   IntPtr tag;
   UInt32 set_index;

   splitAddress(addr, tag, set_index);
   assert(set_index < m_num_sets);

   return m_sets[set_index]->invalidate(tag);
}

CacheBlockInfo*
Cache::accessSingleLine(IntPtr addr, access_t access_type,
      Byte* buff, UInt32 bytes, SubsecondTime now, bool update_replacement)
{
   //assert((buff == NULL) == (bytes == 0));

   IntPtr tag;
   UInt32 set_index;
   UInt32 line_index = (UInt32)-1;
   UInt32 block_offset;

   splitAddress(addr, tag, set_index, block_offset);

   CacheSet* set = m_sets[set_index];
   CacheBlockInfo* cache_block_info = set->find(tag, &line_index);

   if (cache_block_info == NULL)
      return NULL;

   if (access_type == LOAD)
   {
      // NOTE: assumes error occurs in memory. If we want to model bus errors, insert the error into buff instead
      if (m_fault_injector)
         m_fault_injector->preRead(addr, set_index * m_associativity + line_index, bytes, (Byte*)m_sets[set_index]->getDataPtr(line_index, block_offset), now);

      set->read_line(line_index, block_offset, buff, bytes, update_replacement);
   }
   else
   {
      set->write_line(line_index, block_offset, buff, bytes, update_replacement);

      // NOTE: assumes error occurs in memory. If we want to model bus errors, insert the error into buff instead
      if (m_fault_injector)
         m_fault_injector->postWrite(addr, set_index * m_associativity + line_index, bytes, (Byte*)m_sets[set_index]->getDataPtr(line_index, block_offset), now);
   }

   return cache_block_info;
}

void
Cache::insertSingleLine(IntPtr addr, Byte* fill_buff,
      bool* eviction, IntPtr* evict_addr,
      CacheBlockInfo* evict_block_info, Byte* evict_buff,
      SubsecondTime now, CacheCntlr *cntlr)
{
   IntPtr tag;
   UInt32 set_index;
   splitAddress(addr, tag, set_index);

   CacheBlockInfo* cache_block_info = CacheBlockInfo::create(m_cache_type);
   cache_block_info->setTag(tag);

   // Determine which ways are allowed for this insertion.
   // Compose restrictions from (1) per-insert-type masks and (2) hybrid SRAM/MRAM policy.
   UInt32 allowed_mask = 0xFFFFFFFFu;

   // (1) Optional per-insert-type restriction (data vs tag-only)
   if (m_use_hybrid_insert_masks)
   {
      const bool tag_only_insert = (fill_buff == NULL);
      UInt32 by_type = tag_only_insert ? m_hybrid_allowed_way_mask_tag
                                       : m_hybrid_allowed_way_mask_data;

      if (by_type == 0u)
         by_type = 0xFFFFFFFFu; // fallback to unrestricted to avoid deadlock

      allowed_mask &= by_type;
   }

   // (2) Optional hybrid SRAM/MRAM policy restriction
   if (m_hybrid_enabled)
   {
      UInt32 by_tech = m_hybrid_fill_to_mram ? m_mram_way_mask : m_sram_way_mask;
      if (by_tech == 0u)
         by_tech = 0xFFFFFFFFu; // fallback to unrestricted

      allowed_mask &= by_tech;
   }

   if (allowed_mask == 0u)
      allowed_mask = 0xFFFFFFFFu; // final safety fallback

   m_sets[set_index]->insert(cache_block_info, fill_buff,
         eviction, evict_block_info, evict_buff, cntlr, allowed_mask);
   *evict_addr = tagToAddress(evict_block_info->getTag());

   if (m_fault_injector) {
      // NOTE: no callback is generated for read of evicted data
      UInt32 line_index = (UInt32)-1;
      __attribute__((unused)) CacheBlockInfo* res = m_sets[set_index]->find(tag, &line_index);
      LOG_ASSERT_ERROR(res != NULL, "Inserted line no longer there?");

      m_fault_injector->postWrite(addr, set_index * m_associativity + line_index, m_sets[set_index]->getBlockSize(), (Byte*)m_sets[set_index]->getDataPtr(line_index, 0), now);
   }

   #ifdef ENABLE_SET_USAGE_HIST
   ++m_set_usage_hist[set_index];
   #endif

   // Tag the inserted line with its tech (L3 only; SharedCacheBlockInfo)
   if (m_cache_type == CacheBase::SHARED_CACHE) {
      UInt32 line_index = (UInt32)-1;
      CacheBlockInfo* res = m_sets[set_index]->find(tag, &line_index);
      if (res) {
         SharedCacheBlockInfo* sbi = static_cast<SharedCacheBlockInfo*>(res);
         sbi->setTech(m_hybrid_enabled && m_hybrid_fill_to_mram
                         ? SharedCacheBlockInfo::TECH_MRAM
                         : SharedCacheBlockInfo::TECH_SRAM);
      }
   }

   delete cache_block_info;
}


// Single line cache access at addr
CacheBlockInfo*
Cache::peekSingleLine(IntPtr addr)
{
   IntPtr tag;
   UInt32 set_index;
   splitAddress(addr, tag, set_index);

   return m_sets[set_index]->find(tag);
}

void
Cache::updateCounters(bool cache_hit)
{
   if (m_enabled)
   {
      m_num_accesses ++;
      if (cache_hit)
         m_num_hits ++;
   }
}

void
Cache::updateHits(Core::mem_op_t mem_op_type, UInt64 hits)
{
   if (m_enabled)
   {
      m_num_accesses += hits;
      m_num_hits += hits;
   }
}

