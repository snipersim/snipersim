#include "stream_prefetcher.h"
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

Streamer::Streamer(String configName, core_id_t core_id)
   : m_core_id(core_id)
   , m_allocate_on_miss(false)
   , m_num_prefetches(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/streamer/num_prefetches", core_id))
   , m_num_streams(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/streamer/num_streams", core_id))
   , m_prefetch_front(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/streamer/prefetch_front", core_id))
   , m_max_conf(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/streamer/max_conf", core_id))
   , m_conf_thresh(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/streamer/conf_thresh", core_id))
{
   String base = "perf_model/" + configName + "/prefetcher/streamer/";
   if (Sim()->getCfg()->hasKey(base + "allocate_on_miss"))
      m_allocate_on_miss = Sim()->getCfg()->getBoolArray(base + "allocate_on_miss", core_id);

   m_stream_table.resize(m_num_streams);

   memset(&stats, 0, sizeof(stats));
   registerStatsMetric("streamer", m_core_id, "pref_called", &stats.pref_called);
   registerStatsMetric("streamer", m_core_id, "table_hit", &stats.table_hit);
   registerStatsMetric("streamer", m_core_id, "table_replacement", &stats.table_replacement);
   registerStatsMetric("streamer", m_core_id, "incr_conf", &stats.incr_conf);
   registerStatsMetric("streamer", m_core_id, "decr_conf", &stats.decr_conf);
   registerStatsMetric("streamer", m_core_id, "steady", &stats.steady);
   registerStatsMetric("streamer", m_core_id, "gen_prefetch", &stats.gen_prefetch);
}

Streamer::~Streamer()
{
}

std::vector<IntPtr>
Streamer::getNextAddress(IntPtr current_address, core_id_t core_id, Core::mem_op_t mem_op_type, bool cache_hit, bool prefetch_hit, IntPtr eip)
{
   std::vector<IntPtr> pref_addr;
   if (mem_op_type == Core::WRITE)
      return pref_addr;

   IntPtr current_page = current_address >> LOG2_PAGE_SIZE;
   IntPtr current_offset = (current_address >> LOG2_CACHE_BLOCK_SIZE) & CACHE_BLOCK_MASK;

   stats.pref_called++;
   SInt32 index = find(current_page);
   if (index != -1)
   {
      stats.table_hit++;
      StreamEntry &entry = m_stream_table[index];
      if (current_offset != entry.last_offset)
      {
         SInt8 curr_dir = current_offset > entry.last_offset ? 1 : -1;
         if (curr_dir == entry.dir)
         {
            stats.incr_conf++;
            if (entry.conf < m_max_conf)
               entry.conf++;
         }
         else
         {
            stats.decr_conf++;
            if (entry.conf)
               entry.conf--;
         }
         entry.last_offset = current_offset;
         entry.dir = curr_dir;
         updateAge(index);
         if (entry.conf >= m_conf_thresh)
         {
            stats.steady++;
            pref_addr = generatePrefetchAddresses(entry, current_page, current_offset);
         }
      }
   }
   else if (!m_allocate_on_miss || !cache_hit)
   {
      stats.table_replacement++;
      index = findReplacement();
      StreamEntry &entry = m_stream_table[index];
      entry.valid = true;
      entry.page = current_page;
      entry.last_offset = current_offset;
      entry.dir = 0;
      entry.conf = 0;
      updateAge(index);
   }
   return pref_addr;
}

SInt32 Streamer::find(IntPtr page)
{
   for (UInt32 index = 0; index < m_stream_table.size(); ++index)
      if (m_stream_table[index].valid && m_stream_table[index].page == page)
         return index;
   return -1;
}

void Streamer::updateAge(SInt32 current)
{
   for (UInt32 index = 0; index < m_stream_table.size(); ++index)
   {
      if ((SInt32)index == current)
         m_stream_table[index].lru = 0;
      else
         m_stream_table[index].lru++;
   }
}

SInt32 Streamer::findReplacement()
{
   UInt32 max_lru = 0;
   SInt32 replacement_index = 0;
   for (UInt32 index = 0; index < m_stream_table.size(); ++index)
   {
      if (!m_stream_table[index].valid)
         return index;
      if (m_stream_table[index].lru >= max_lru)
      {
         max_lru = m_stream_table[index].lru;
         replacement_index = index;
      }
   }
   return replacement_index;
}

std::vector<IntPtr> Streamer::generatePrefetchAddresses(const StreamEntry &entry, IntPtr current_page, IntPtr current_offset)
{
   std::vector<IntPtr> addresses;
   for (UInt32 index = 1; index <= m_num_prefetches; ++index)
   {
      SInt32 prefetch_offset = (SInt32)current_offset + (SInt32)(m_prefetch_front + index) * entry.dir;
      if (prefetch_offset >= 0 && prefetch_offset < NUM_CACHE_BLOCKS_IN_PAGE)
      {
         IntPtr prefetch_address = (current_page << LOG2_PAGE_SIZE) + ((IntPtr)prefetch_offset << LOG2_CACHE_BLOCK_SIZE);
         addresses.push_back(prefetch_address);
         stats.gen_prefetch++;
      }
   }
   return addresses;
}
