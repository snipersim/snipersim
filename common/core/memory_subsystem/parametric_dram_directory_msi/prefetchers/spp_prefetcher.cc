#include "spp_prefetcher.h"
#include "simulator.h"
#include "config.hpp"
#include "stats.h"

#include <cstring>

SPPPrefetcher::SPPPrefetcher(String configName, core_id_t core_id)
   : m_core_id(core_id)
   , m_lookahead_on(true)
   , m_filter_on(true)
   , m_ghr_on(true)
{
   // All three are optional and on by default, as upstream: a config that
   // selects spp need set nothing else.
   String base = "perf_model/" + configName + "/prefetcher/spp/";
   if (Sim()->getCfg()->hasKey(base + "lookahead_on"))
      m_lookahead_on = Sim()->getCfg()->getBoolArray(base + "lookahead_on", core_id);
   if (Sim()->getCfg()->hasKey(base + "filter_on"))
      m_filter_on = Sim()->getCfg()->getBoolArray(base + "filter_on", core_id);
   if (Sim()->getCfg()->hasKey(base + "ghr_on"))
      m_ghr_on = Sim()->getCfg()->getBoolArray(base + "ghr_on", core_id);

   memset(&stats, 0, sizeof(stats));
   registerStatsMetric("spp", m_core_id, "pref_called", &stats.pref_called);
   registerStatsMetric("spp", m_core_id, "prefetches_issued", &stats.prefetches_issued);
   registerStatsMetric("spp", m_core_id, "st_hits", &stats.st_hits);
   registerStatsMetric("spp", m_core_id, "st_misses", &stats.st_misses);
   registerStatsMetric("spp", m_core_id, "filter_hits", &stats.filter_hits);
}

SPPPrefetcher::~SPPPrefetcher()
{
}

std::vector<IntPtr>
SPPPrefetcher::getNextAddress(IntPtr current_address, core_id_t core_id, Core::mem_op_t mem_op_type, bool cache_hit, bool prefetch_hit, IntPtr eip)
{
   std::vector<IntPtr> pref_addr;
   if (mem_op_type == Core::WRITE)
      return pref_addr;

   stats.pref_called++;

   IntPtr page = current_address >> LOG2_PAGE_SIZE;

   UInt32 last_sig = 0, curr_sig = 0, depth = 0;
   SInt32 delta = 0;

   std::vector<UInt32> confidence_q(PF_QUEUE, 0);
   std::vector<SInt32> delta_q(PF_QUEUE, 0);

   confidence_q[0] = 100;
   GHR.global_accuracy = GHR.pf_issued ? ((100 * GHR.pf_useful) / GHR.pf_issued) : 0;

   // Stage 1: read and update the signature stored in the ST.  (Upstream
   // registered st_hits/st_misses and never counted them.)
   if (ST.readAndUpdateSig(current_address, last_sig, curr_sig, delta))
      stats.st_hits++;
   else
      stats.st_misses++;

   // The filter learns which demand lines a prefetch brought in
   if (m_filter_on)
      FILTER.check(current_address, L2C_DEMAND, GHR.pf_useful, GHR.pf_issued);

   // Stage 2: update the delta pattern stored in the PT
   if (last_sig)
      PT.updatePattern(last_sig, delta);

   // Stage 3: prefetch along the signature path
   IntPtr base_addr = current_address;
   UInt32 lookahead_conf = 100, pf_q_head = 0, pf_q_tail = 0;
   bool do_lookahead = false;

   do
   {
      UInt32 lookahead_way = PT_WAY;
      PT.readPattern(curr_sig, delta_q, confidence_q, lookahead_way, lookahead_conf, pf_q_tail, depth);

      do_lookahead = false;
      for (UInt32 i = pf_q_head; i < pf_q_tail; i++)
      {
         if (confidence_q[i] >= PF_THRESHOLD)
         {
            IntPtr pf_block = (base_addr >> LOG2_BLOCK_SIZE) + delta_q[i];
            IntPtr pf_address = pf_block << LOG2_BLOCK_SIZE;
            IntPtr pf_page = pf_address >> LOG2_PAGE_SIZE;

            if (pf_page == page)
            {
               bool should_prefetch = true;
               if (m_filter_on)
                  should_prefetch = FILTER.check(pf_address,
                     (confidence_q[i] >= FILL_THRESHOLD) ? SPP_L2C_PREFETCH : SPP_LLC_PREFETCH,
                     GHR.pf_useful, GHR.pf_issued);

               if (should_prefetch)
               {
                  pref_addr.push_back(pf_address);
                  stats.prefetches_issued++;

                  if (confidence_q[i] >= FILL_THRESHOLD)
                  {
                     GHR.pf_issued++;
                     if (GHR.pf_issued > GLOBAL_COUNTER_MAX)
                     {
                        GHR.pf_issued >>= 1;
                        GHR.pf_useful >>= 1;
                     }
                  }
               }
               else
                  stats.filter_hits++;
            }
            else if (m_ghr_on)
            {
               // Crosses the page: remembered for bootstrapping the next one
               UInt32 pf_offset = (pf_address >> LOG2_BLOCK_SIZE) & (BLOCKS_PER_PAGE - 1);
               GHR.updateEntry(curr_sig, confidence_q[i], pf_offset, delta_q[i]);
            }

            do_lookahead = true;
            pf_q_head++;
         }
      }

      // Step the base address and signature along the most confident delta
      if (lookahead_way < PT_WAY)
      {
         UInt32 set = hash(curr_sig) % PT_SET;
         base_addr += (IntPtr)((SInt64)PT.delta[set][lookahead_way] << LOG2_BLOCK_SIZE);

         SInt32 d = PT.delta[set][lookahead_way];
         UInt32 sig_delta = (d < 0) ? ((UInt32)(-d) + (1 << (SIG_DELTA_BIT - 1))) : (UInt32)d;
         curr_sig = ((curr_sig << SIG_SHIFT) ^ sig_delta) & SIG_MASK;
      }
   } while (m_lookahead_on && do_lookahead);

   return pref_addr;
}

UInt64 SPPPrefetcher::hash(UInt64 key)
{
   // Robert Jenkins' 32-bit mix, then Knuth's multiplicative step
   key += (key << 12);
   key ^= (key >> 22);
   key += (key << 4);
   key ^= (key >> 9);
   key += (key << 10);
   key ^= (key >> 2);
   key += (key << 7);
   key ^= (key >> 12);
   key = (key >> 3) * 2654435761ULL;
   return key;
}

// Signature table

SPPPrefetcher::SignatureTable::SignatureTable()
{
   for (UInt32 set = 0; set < ST_SET; set++)
      for (UInt32 way = 0; way < ST_WAY; way++)
      {
         valid[set][way] = false;
         tag[set][way] = 0;
         last_offset[set][way] = 0;
         sig[set][way] = 0;
         lru[set][way] = way;
      }
}

bool SPPPrefetcher::SignatureTable::readAndUpdateSig(IntPtr addr, UInt32 &last_sig, UInt32 &curr_sig, SInt32 &delta)
{
   IntPtr page = addr >> LOG2_PAGE_SIZE;
   UInt32 page_offset = (addr >> LOG2_BLOCK_SIZE) & (BLOCKS_PER_PAGE - 1);

   UInt32 set = hash(page) % ST_SET;
   UInt64 partial_page = page & ((1ULL << ST_TAG_BIT) - 1);

   UInt32 match = ST_WAY;
   bool st_hit = false;

   for (UInt32 way = 0; way < ST_WAY; way++)
   {
      if (valid[set][way] && tag[set][way] == partial_page)
      {
         match = way;
         last_sig = sig[set][way];
         delta = (SInt32)page_offset - (SInt32)last_offset[set][way];

         if (delta != 0)
         {
            UInt32 sig_delta = (delta < 0) ? ((UInt32)(-delta) + (1 << (SIG_DELTA_BIT - 1))) : (UInt32)delta;
            sig[set][way] = ((last_sig << SIG_SHIFT) ^ sig_delta) & SIG_MASK;
            curr_sig = sig[set][way];
            last_offset[set][way] = page_offset;
         }
         else
            last_sig = 0;   // same line again: nothing to learn
         st_hit = true;
         break;
      }
   }
   const bool tag_matched = st_hit;

   if (!st_hit)
   {
      for (UInt32 way = 0; way < ST_WAY; way++)
      {
         if (!valid[set][way])
         {
            match = way;
            valid[set][way] = true;
            tag[set][way] = partial_page;
            sig[set][way] = 0;
            curr_sig = 0;
            last_offset[set][way] = page_offset;
            st_hit = true;
            break;
         }
      }
   }

   if (!st_hit)
   {
      for (UInt32 way = 0; way < ST_WAY; way++)
      {
         if (lru[set][way] == ST_WAY - 1)
         {
            match = way;
            tag[set][way] = partial_page;
            sig[set][way] = 0;
            curr_sig = 0;
            last_offset[set][way] = page_offset;
            break;
         }
      }
   }

   if (match < ST_WAY)
   {
      for (UInt32 way = 0; way < ST_WAY; way++)
         if (lru[set][way] < lru[set][match])
            lru[set][way]++;
      lru[set][match] = 0;
   }
   return tag_matched;
}

// Pattern table

SPPPrefetcher::PatternTable::PatternTable()
{
   for (UInt32 set = 0; set < PT_SET; set++)
   {
      for (UInt32 way = 0; way < PT_WAY; way++)
      {
         delta[set][way] = 0;
         c_delta[set][way] = 0;
      }
      c_sig[set] = 0;
   }
}

void SPPPrefetcher::PatternTable::updatePattern(UInt32 last_sig, SInt32 curr_delta)
{
   UInt32 set = hash(last_sig) % PT_SET;

   for (UInt32 way = 0; way < PT_WAY; way++)
   {
      if (delta[set][way] == curr_delta)
      {
         if (c_delta[set][way] < C_DELTA_MAX)
            c_delta[set][way]++;
         if (c_sig[set] < C_SIG_MAX)
            c_sig[set]++;
         return;
      }
   }

   // Replace the least confident delta
   UInt32 victim_way = 0;
   UInt32 min_counter = c_delta[set][0];
   for (UInt32 way = 1; way < PT_WAY; way++)
   {
      if (c_delta[set][way] < min_counter)
      {
         min_counter = c_delta[set][way];
         victim_way = way;
      }
   }

   delta[set][victim_way] = curr_delta;
   c_delta[set][victim_way] = 0;
   if (c_sig[set] < C_SIG_MAX)
      c_sig[set]++;
}

void SPPPrefetcher::PatternTable::readPattern(UInt32 curr_sig, std::vector<SInt32> &prefetch_delta, std::vector<UInt32> &confidence_q,
                                              UInt32 &lookahead_way, UInt32 &lookahead_conf, UInt32 &pf_q_tail, UInt32 &depth)
{
   UInt32 set = hash(curr_sig) % PT_SET;
   if (c_sig[set] == 0)
      return;

   for (UInt32 way = 0; way < PT_WAY; way++)
   {
      if (c_delta[set][way] > 0 && pf_q_tail < prefetch_delta.size())
      {
         UInt32 conf = (100 * c_delta[set][way]) / c_sig[set];
         if (conf >= PF_THRESHOLD)
         {
            prefetch_delta[pf_q_tail] = delta[set][way];
            confidence_q[pf_q_tail] = conf;
            if (conf > lookahead_conf)
            {
               lookahead_conf = conf;
               lookahead_way = way;
            }
            pf_q_tail++;
            depth++;
         }
      }
   }
}

// Prefetch filter

SPPPrefetcher::PrefetchFilter::PrefetchFilter()
{
   for (UInt32 set = 0; set < FILTER_SET; set++)
   {
      remainder_tag[set] = 0;
      valid[set] = false;
      useful[set] = false;
   }
}

bool SPPPrefetcher::PrefetchFilter::check(IntPtr pf_addr, FilterRequest filter_request, UInt32 &pf_useful_counter, UInt32 &pf_issued_counter)
{
   UInt64 cache_line = pf_addr >> LOG2_BLOCK_SIZE;
   UInt64 h = hash(cache_line);
   UInt64 quotient = (h >> REMAINDER_BIT) & ((1 << QUOTIENT_BIT) - 1);
   UInt64 remainder = h & ((1 << REMAINDER_BIT) - 1);

   switch (filter_request)
   {
      case SPP_L2C_PREFETCH:
      case SPP_LLC_PREFETCH:
         if (valid[quotient] && remainder_tag[quotient] == remainder)
            return false;   // already prefetched
         valid[quotient] = true;
         useful[quotient] = false;
         remainder_tag[quotient] = remainder;
         return true;

      case L2C_DEMAND:
         if (valid[quotient] && remainder_tag[quotient] == remainder)
         {
            if (!useful[quotient])
            {
               useful[quotient] = true;
               if (pf_useful_counter < GLOBAL_COUNTER_MAX)
                  pf_useful_counter++;
            }
         }
         return false;

      case L2C_EVICT:
         if (valid[quotient] && remainder_tag[quotient] == remainder)
            valid[quotient] = false;
         return false;
   }
   return false;
}

// Global history register

SPPPrefetcher::GlobalRegister::GlobalRegister()
   : pf_useful(0), pf_issued(0), global_accuracy(0)
{
   for (UInt32 i = 0; i < MAX_GHR_ENTRY; i++)
   {
      valid[i] = false;
      sig[i] = 0;
      confidence[i] = 0;
      offset[i] = 0;
      delta[i] = 0;
   }
}

void SPPPrefetcher::GlobalRegister::updateEntry(UInt32 pf_sig, UInt32 pf_confidence, UInt32 pf_offset, SInt32 pf_delta)
{
   // An invalid entry, else the least confident one
   UInt32 victim = 0;
   for (UInt32 i = 0; i < MAX_GHR_ENTRY; i++)
   {
      if (!valid[i])
      {
         victim = i;
         break;
      }
      if (confidence[i] < confidence[victim])
         victim = i;
   }

   valid[victim] = true;
   sig[victim] = pf_sig;
   confidence[victim] = pf_confidence;
   offset[victim] = pf_offset;
   delta[victim] = pf_delta;
}
