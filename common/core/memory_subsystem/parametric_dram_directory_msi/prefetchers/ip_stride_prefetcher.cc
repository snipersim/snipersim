#include "ip_stride_prefetcher.h"
#include "simulator.h"
#include "config.hpp"
#include "stats.h"

#include <cstring>

namespace
{
   const IntPtr PAGE_SIZE = 4096;
   const IntPtr CACHE_BLOCK_SIZE = 64;
   const IntPtr LOG2_PAGE_SIZE = 12;
   const IntPtr LOG2_CACHE_BLOCK_SIZE = 6;
   const IntPtr CACHE_BLOCK_MASK = (PAGE_SIZE / CACHE_BLOCK_SIZE) - 1;
   const SInt32 NUM_CACHE_BLOCKS_IN_PAGE = PAGE_SIZE / CACHE_BLOCK_SIZE;
}

IPStridePrefetcher::IPStridePrefetcher(String configName, core_id_t core_id)
   : m_core_id(core_id)
   , m_allocate_on_miss(false)
   , m_num_prefetches(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/ip_stride/num_prefetches", core_id))
   , m_num_streams(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/ip_stride/num_streams", core_id))
   , m_stop_at_page(true)
   , m_lookahead(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/ip_stride/lookahead", core_id))
{
   // Optional, as upstream: the core configs that select ip_stride set both,
   // the machine configs that do not select it carry neither.
   String base = "perf_model/" + configName + "/prefetcher/ip_stride/";
   if (Sim()->getCfg()->hasKey(base + "allocate_on_miss"))
      m_allocate_on_miss = Sim()->getCfg()->getBoolArray(base + "allocate_on_miss", core_id);
   if (Sim()->getCfg()->hasKey(base + "stop_at_page_boundary"))
      m_stop_at_page = Sim()->getCfg()->getBoolArray(base + "stop_at_page_boundary", core_id);

   m_rpt.resize(m_num_streams);

   memset(&stats, 0, sizeof(stats));
   registerStatsMetric("ip_stride", m_core_id, "pref_called", &stats.pref_called);
   registerStatsMetric("ip_stride", m_core_id, "rpt_hit", &stats.rpt_hit);
   registerStatsMetric("ip_stride", m_core_id, "non_zero_curr_stride", &stats.non_zero_curr_stride);
   registerStatsMetric("ip_stride", m_core_id, "curr_stride_correct", &stats.curr_stride_correct);
   registerStatsMetric("ip_stride", m_core_id, "steady", &stats.steady);
   registerStatsMetric("ip_stride", m_core_id, "rpt_replacement", &stats.rpt_replacement);
   registerStatsMetric("ip_stride", m_core_id, "gen_prefetch", &stats.gen_prefetch);
}

IPStridePrefetcher::~IPStridePrefetcher()
{
}

std::vector<IntPtr>
IPStridePrefetcher::getNextAddress(IntPtr current_address, core_id_t core_id, Core::mem_op_t mem_op_type, bool cache_hit, bool prefetch_hit, IntPtr eip)
{
   std::vector<IntPtr> pref_addr;
   if (mem_op_type == Core::WRITE)
      return pref_addr;

   IntPtr current_page = current_address >> LOG2_PAGE_SIZE;
   IntPtr current_offset = (current_address >> LOG2_CACHE_BLOCK_SIZE) & CACHE_BLOCK_MASK;

   stats.pref_called++;
   SInt32 index = find(eip);
   if (index != -1)
   {
      stats.rpt_hit++;
      RPTEntry &entry = m_rpt[index];
      SInt32 curr_stride = (SInt32)current_offset - (SInt32)entry.prev_offset;

      if (curr_stride)
      {
         stats.non_zero_curr_stride++;
         if (curr_stride == entry.stride)
         {
            stats.curr_stride_correct++;
            if (entry.state == INIT || entry.state == TRANSIENT || entry.state == STEADY)
               entry.state = STEADY;
            else if (entry.state == NO_PRED)
               entry.state = TRANSIENT;
         }
         else
         {
            if (entry.state == INIT)
               entry.state = TRANSIENT;
            else if (entry.state == STEADY)
               entry.state = INIT;
            else if (entry.state == TRANSIENT || entry.state == NO_PRED)
               entry.state = NO_PRED;
         }

         entry.prev_offset = current_offset;
         entry.stride = curr_stride;
         updateAge(index);

         if (entry.state == STEADY)
         {
            stats.steady++;
            pref_addr = generatePrefetchAddresses(entry, current_page, current_offset);
         }
      }
   }
   else if (!m_allocate_on_miss || !cache_hit)
   {
      stats.rpt_replacement++;
      index = findReplacement();
      RPTEntry &entry = m_rpt[index];
      entry.valid = true;
      entry.eip = eip;
      entry.prev_offset = current_offset;
      entry.stride = 0;
      entry.state = INIT;
      updateAge(index);
   }

   return pref_addr;
}

SInt32 IPStridePrefetcher::find(IntPtr eip)
{
   for (UInt32 index = 0; index < m_rpt.size(); ++index)
      if (m_rpt[index].valid && m_rpt[index].eip == eip)
         return index;
   return -1;
}

void IPStridePrefetcher::updateAge(SInt32 current)
{
   for (UInt32 index = 0; index < m_rpt.size(); ++index)
   {
      if ((SInt32)index == current)
         m_rpt[index].lru = 0;
      else
         m_rpt[index].lru++;
   }
}

SInt32 IPStridePrefetcher::findReplacement()
{
   UInt32 max_lru = 0;
   SInt32 replacement_index = 0;
   for (UInt32 index = 0; index < m_rpt.size(); ++index)
   {
      if (!m_rpt[index].valid)
         return index;
      if (m_rpt[index].lru >= max_lru)
      {
         max_lru = m_rpt[index].lru;
         replacement_index = index;
      }
   }
   return replacement_index;
}

std::vector<IntPtr> IPStridePrefetcher::generatePrefetchAddresses(const RPTEntry &entry, IntPtr current_page, IntPtr current_offset)
{
   std::vector<IntPtr> addresses;
   for (UInt32 index = 1; index <= m_num_prefetches; ++index)
   {
      SInt32 prefetch_offset = (SInt32)current_offset + (SInt32)(m_lookahead + index) * entry.stride;
      if (!m_stop_at_page || (prefetch_offset >= 0 && prefetch_offset < NUM_CACHE_BLOCKS_IN_PAGE))
      {
         // Past the page the offset is a signed line count from the page base.
         IntPtr prefetch_address = (current_page << LOG2_PAGE_SIZE) + (IntPtr)((SInt64)prefetch_offset * CACHE_BLOCK_SIZE);
         addresses.push_back(prefetch_address);
         stats.gen_prefetch++;
      }
   }
   return addresses;
}
