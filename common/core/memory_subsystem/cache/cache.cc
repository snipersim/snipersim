#include "simulator.h"
#include "cache.h"
#include "log.h"
#include "config.hpp"
#include "shared_cache_block_info.h"
#include <sstream>

bool Cache::decideLineTech(IntPtr addr, UInt32 set_index, bool *to_mram) const
{
   if (!m_hybrid_enabled) return false;

   // First, range rules (highest priority)
   if (!m_addr_rules.empty())
   {
      for (const auto &r : m_addr_rules)
      {
         // Compare as unsigned
         
	      const IntPtr addr_ip = addr;
const IntPtr start_ip = r.start;
const IntPtr end_ip   = r.end;
	     if (addr_ip >= start_ip && addr_ip < end_ip) { 
            if (to_mram) *to_mram = r.to_mram;
            return true;
         }
      }
   }

   // Second, set-parity mapping
   if (m_map_use_set_parity)
   {
      const bool is_even = ((set_index & 1u) == 0u);
      if (to_mram) *to_mram = m_map_even_sets_are_mram ? is_even : !is_even;
      return true;
   }

   return false; // Fall back to global fill_to
}

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

   // ---- Optional per-line mapping knobs ----
   //  perf_model/.../hybrid/line_map/mode = "none" | "set-parity" | "ranges"
   // When mode == "set-parity":
   //  perf_model/.../hybrid/line_map/set_parity = "even_is_mram" | "odd_is_mram"
   // When mode == "ranges":
   //  perf_model/.../hybrid/line_map/ranges = "0xA-0xB:mram;0xB-0xC:sram;..."
   // Addresses use C-style integer literals (hex 0x..., or decimal).
   String map_mode = "none";
   if (Sim()->getCfg()->hasKey(hyb_base + "/line_map/mode"))
      map_mode = Sim()->getCfg()->getString(hyb_base + "/line_map/mode");

   // Reset defaults
   m_map_use_set_parity = false;
   m_map_even_sets_are_mram = false;
   m_addr_rules.clear();

   if (map_mode == "set-parity")
   {
      m_map_use_set_parity = true;
      String sp = "even_is_mram";
      String k_sp = hyb_base + "/line_map/set_parity";
      if (Sim()->getCfg()->hasKey(k_sp))
         sp = Sim()->getCfg()->getString(k_sp);
      m_map_even_sets_are_mram = (sp == "even_is_mram");
   }
   else if (map_mode == "ranges")
   {
      String rs;
      String k_rs = hyb_base + "/line_map/ranges";
      if (Sim()->getCfg()->hasKey(k_rs))
         rs = Sim()->getCfg()->getString(k_rs);

      // Parse: "<start>-<end>:<tech> ; ..." (semicolon separated)
      // <tech> = "mram" | "sram"
      std::stringstream ss(rs.c_str());
      String item;
      while (std::getline(ss, item, ';'))
      {
         // Strip spaces
         auto trim = [](String s)->String {
            size_t a = s.find_first_not_of(" \t");
            size_t b = s.find_last_not_of(" \t");
            if (a == String::npos) return "";
            return s.substr(a, b - a + 1);
         };
         item = trim(item);
         if (item.empty()) continue;

         size_t colon = item.find(':');
         if (colon == String::npos) continue;
         String range = trim(item.substr(0, colon));
         String tech  = trim(item.substr(colon + 1));

         size_t dash = range.find('-');
         if (dash == String::npos) continue;
         String s_lo = trim(range.substr(0, dash));
         String s_hi = trim(range.substr(dash + 1));

         // strtoull handles "0x..." or decimal
         char *endp = NULL;
         UInt64 lo = strtoull(s_lo.c_str(), &endp, 0);
         UInt64 hi = strtoull(s_hi.c_str(), &endp, 0);
         if (hi > lo)
         {
            RangeRule r;
            r.start = (IntPtr)lo;
            r.end   = (IntPtr)hi;
            r.to_mram = (tech == "mram");
            m_addr_rules.push_back(r);
         }
      }
   }
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
   bool desired_mram = m_hybrid_fill_to_mram; // default global fill target
   if (m_hybrid_enabled)
   {
      // Consult per-line mapping (ranges or set parity). If it returns true,
      // it overrides the default global fill_to target.
      bool perline_mram = desired_mram;
      if (decideLineTech(addr, set_index, &perline_mram))
         desired_mram = perline_mram;

      // Derive mask for the desired tech, with graceful fallback if empty
      UInt32 by_tech = desired_mram ? m_mram_way_mask : m_sram_way_mask;
      if (by_tech == 0u) {
         // Try the other tech if desired set is exhausted/unavailable
         by_tech = desired_mram ? m_sram_way_mask : m_mram_way_mask;
         if (by_tech == 0u)
            by_tech = 0xFFFFFFFFu; // final fallback
      }
      allowed_mask &= by_tech;
   }

   if (allowed_mask == 0u)
      allowed_mask = 0xFFFFFFFFu; // final safety fallback

   m_sets[set_index]->insert(cache_block_info, fill_buff,
         eviction, evict_block_info, evict_buff, cntlr, allowed_mask);
   *evict_addr = tagToAddress(evict_block_info->getTag());

   if (m_fault_injector) {
      UInt32 line_index = (UInt32)-1;
      __attribute__((unused)) CacheBlockInfo* res = m_sets[set_index]->find(tag, &line_index);
      LOG_ASSERT_ERROR(res != NULL, "Inserted line no longer there?");

      m_fault_injector->postWrite(addr, set_index * m_associativity + line_index, m_sets[set_index]->getBlockSize(), (Byte*)m_sets[set_index]->getDataPtr(line_index, 0), now);
   }

   #ifdef ENABLE_SET_USAGE_HIST
   ++m_set_usage_hist[set_index];
   #endif

   // Tag the inserted line with its actual tech (based on chosen way index)
   if (m_cache_type == CacheBase::SHARED_CACHE) {
      UInt32 line_index = (UInt32)-1;
      CacheBlockInfo* res = m_sets[set_index]->find(tag, &line_index);
      if (res) {
         SharedCacheBlockInfo* sbi = static_cast<SharedCacheBlockInfo*>(res);
         bool is_mram_actual = false;
         if (m_hybrid_enabled) {
            const UInt32 bit = (line_index < 32) ? (1u << line_index) : 0u;
            is_mram_actual = (bit != 0u) && ((m_mram_way_mask & bit) != 0u);
         }
         sbi->setTech(is_mram_actual ? SharedCacheBlockInfo::TECH_MRAM
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
