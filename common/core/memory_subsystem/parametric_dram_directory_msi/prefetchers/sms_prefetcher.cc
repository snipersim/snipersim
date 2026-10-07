#include "sms_prefetcher.h"
#include "simulator.h"
#include "config.hpp"
#include "stats.h"
#include "log.h"

#include <algorithm>
#include <cstring>

namespace
{
   UInt32 cfgInt(const String &base, const char *key, core_id_t core_id, UInt32 def)
   {
      return Sim()->getCfg()->hasKey(base + key) ? Sim()->getCfg()->getIntArray(base + key, core_id) : def;
   }
}

SMSPrefetcher::SMSPrefetcher(String configName, core_id_t core_id)
   : m_core_id(core_id)
{
   // Every knob is optional; the defaults are the ChampSim ones upstream
   // compiled in.
   String base = "perf_model/" + configName + "/prefetcher/sms/";
   m_at_size = cfgInt(base, "at_size", core_id, 32);
   m_ft_size = cfgInt(base, "ft_size", core_id, 64);
   m_pht_size = cfgInt(base, "pht_size", core_id, 2048);
   m_pht_assoc = cfgInt(base, "pht_assoc", core_id, 16);
   m_pref_degree = cfgInt(base, "pref_degree", core_id, 4);
   m_region_size = cfgInt(base, "region_size", core_id, 2048);
   m_pref_buffer_size = cfgInt(base, "pref_buffer_size", core_id, 256);

   m_region_size_log = 0;
   while ((1U << m_region_size_log) < m_region_size)
      m_region_size_log++;
   LOG_ASSERT_ERROR((1U << m_region_size_log) == m_region_size, "SMS region_size (%u) must be a power of two", m_region_size);
   LOG_ASSERT_ERROR(m_region_size_log > LOG2_BLOCK_SIZE && (m_region_size >> LOG2_BLOCK_SIZE) <= BITMAP_MAX_SIZE,
                    "SMS region_size (%u) must be between 2 and %u lines", m_region_size, BITMAP_MAX_SIZE);
   LOG_ASSERT_ERROR(m_pht_assoc > 0 && m_pht_size >= m_pht_assoc, "SMS pht_size (%u) must hold at least one set of pht_assoc (%u)", m_pht_size, m_pht_assoc);

   m_pht.resize(m_pht_size / m_pht_assoc);

   memset(&stats, 0, sizeof(stats));
   registerStatsMetric("sms", m_core_id, "pref_called", &stats.pref_called);
   registerStatsMetric("sms", m_core_id, "ft_hits", &stats.ft_hits);
   registerStatsMetric("sms", m_core_id, "at_hits", &stats.at_hits);
   registerStatsMetric("sms", m_core_id, "pht_hits", &stats.pht_hits);
   registerStatsMetric("sms", m_core_id, "prefetches_generated", &stats.prefetches_generated);
   registerStatsMetric("sms", m_core_id, "prefetches_issued", &stats.prefetches_issued);
}

SMSPrefetcher::~SMSPrefetcher()
{
}

std::vector<IntPtr>
SMSPrefetcher::getNextAddress(IntPtr current_address, core_id_t core_id, Core::mem_op_t mem_op_type, bool cache_hit, bool prefetch_hit, IntPtr eip)
{
   // A write trains nothing but still drains the buffer
   if (mem_op_type == Core::WRITE)
      return issuePrefetches();

   stats.pref_called++;

   UInt64 page = current_address >> m_region_size_log;
   UInt32 offset = (current_address >> LOG2_BLOCK_SIZE) & ((1ULL << (m_region_size_log - LOG2_BLOCK_SIZE)) - 1);

   std::deque<ATEntry>::iterator at = findAcc(page);
   if (at != m_acc_table.end())
   {
      stats.at_hits++;
      at->pattern[offset] = 1;
      touchAcc(at);
   }
   else
   {
      std::deque<FTEntry>::iterator ft = findFilter(page);
      if (ft != m_filter_table.end())
      {
         // Second access to the region: start accumulating
         stats.ft_hits++;
         insertAcc(*ft, offset);
         m_filter_table.erase(ft);
      }
      else
      {
         // First access: this is the trigger, replay what it did last time
         insertFilter(eip, page, offset);
         generatePrefetches(eip, page, offset);
      }
   }

   return issuePrefetches();
}

// Filter table (FIFO)

std::deque<SMSPrefetcher::FTEntry>::iterator SMSPrefetcher::findFilter(UInt64 page)
{
   for (std::deque<FTEntry>::iterator it = m_filter_table.begin(); it != m_filter_table.end(); ++it)
      if (it->page == page)
         return it;
   return m_filter_table.end();
}

void SMSPrefetcher::insertFilter(UInt64 pc, UInt64 page, UInt32 offset)
{
   if (m_filter_table.size() >= m_ft_size)
      m_filter_table.pop_front();
   FTEntry entry;
   entry.page = page;
   entry.pc = pc;
   entry.trigger_offset = offset;
   m_filter_table.push_back(entry);
}

// Accumulation table (LRU by age)

std::deque<SMSPrefetcher::ATEntry>::iterator SMSPrefetcher::findAcc(UInt64 page)
{
   for (std::deque<ATEntry>::iterator it = m_acc_table.begin(); it != m_acc_table.end(); ++it)
      if (it->page == page)
         return it;
   return m_acc_table.end();
}

void SMSPrefetcher::insertAcc(const FTEntry &ft, UInt32 offset)
{
   if (m_acc_table.size() >= m_at_size)
   {
      UInt32 max_age = 0;
      std::deque<ATEntry>::iterator victim = m_acc_table.begin();
      for (std::deque<ATEntry>::iterator it = m_acc_table.begin(); it != m_acc_table.end(); ++it)
         if (it->age >= max_age)
         {
            max_age = it->age;
            victim = it;
         }
      evictAcc(victim);
   }

   ATEntry entry;
   entry.pc = ft.pc;
   entry.page = ft.page;
   entry.trigger_offset = ft.trigger_offset;
   entry.pattern.reset();
   entry.pattern[ft.trigger_offset] = 1;
   entry.pattern[offset] = 1;
   entry.age = 0;

   for (std::deque<ATEntry>::iterator it = m_acc_table.begin(); it != m_acc_table.end(); ++it)
      it->age++;
   m_acc_table.push_back(entry);
}

void SMSPrefetcher::evictAcc(std::deque<ATEntry>::iterator victim)
{
   // The pattern the region accumulated is what the PHT learns
   insertPHT(*victim);
   m_acc_table.erase(victim);
}

void SMSPrefetcher::touchAcc(std::deque<ATEntry>::iterator current)
{
   for (std::deque<ATEntry>::iterator it = m_acc_table.begin(); it != m_acc_table.end(); ++it)
      it->age++;
   current->age = 0;
}

// Pattern history table (set-associative, LRU by age)

void SMSPrefetcher::insertPHT(const ATEntry &at)
{
   UInt64 sig = signature(at.pc, at.trigger_offset);
   UInt32 set = 0;
   std::deque<PHTEntry>::iterator it = findPHT(sig, set);
   if (it != m_pht[set].end())
   {
      stats.pht_hits++;
      it->pattern = at.pattern;
      touchPHT(set, it);
      return;
   }

   if (m_pht[set].size() >= m_pht_assoc)
   {
      UInt32 max_age = 0;
      std::deque<PHTEntry>::iterator victim = m_pht[set].begin();
      for (std::deque<PHTEntry>::iterator v = m_pht[set].begin(); v != m_pht[set].end(); ++v)
         if (v->age >= max_age)
         {
            max_age = v->age;
            victim = v;
         }
      m_pht[set].erase(victim);
   }

   PHTEntry entry;
   entry.signature = sig;
   entry.pattern = at.pattern;
   entry.age = 0;
   for (std::deque<PHTEntry>::iterator e = m_pht[set].begin(); e != m_pht[set].end(); ++e)
      e->age++;
   m_pht[set].push_back(entry);
}

std::deque<SMSPrefetcher::PHTEntry>::iterator SMSPrefetcher::findPHT(UInt64 sig, UInt32 &set)
{
   set = sig % m_pht.size();
   for (std::deque<PHTEntry>::iterator it = m_pht[set].begin(); it != m_pht[set].end(); ++it)
      if (it->signature == sig)
         return it;
   return m_pht[set].end();
}

void SMSPrefetcher::touchPHT(UInt32 set, std::deque<PHTEntry>::iterator current)
{
   for (std::deque<PHTEntry>::iterator it = m_pht[set].begin(); it != m_pht[set].end(); ++it)
      it->age++;
   current->age = 0;
}

// Prefetch generation

UInt64 SMSPrefetcher::signature(UInt64 pc, UInt32 offset) const
{
   return (pc << (m_region_size_log - LOG2_BLOCK_SIZE)) + offset;
}

void SMSPrefetcher::generatePrefetches(UInt64 pc, UInt64 page, UInt32 offset)
{
   UInt32 set = 0;
   std::deque<PHTEntry>::iterator it = findPHT(signature(pc, offset), set);
   if (it == m_pht[set].end())
      return;

   stats.pht_hits++;
   const UInt32 lines = m_region_size >> LOG2_BLOCK_SIZE;
   for (UInt32 i = 0; i < lines; ++i)
   {
      if (it->pattern[i] && i != offset)
      {
         if (m_pref_buffer.size() >= m_pref_buffer_size)
            break;
         m_pref_buffer.push_back((page << m_region_size_log) + ((IntPtr)i << LOG2_BLOCK_SIZE));
         stats.prefetches_generated++;
      }
   }
   touchPHT(set, it);
}

std::vector<IntPtr> SMSPrefetcher::issuePrefetches()
{
   std::vector<IntPtr> issued;
   while (!m_pref_buffer.empty() && issued.size() < m_pref_degree)
   {
      issued.push_back(m_pref_buffer.front());
      m_pref_buffer.pop_front();
      stats.prefetches_issued++;
   }
   return issued;
}
