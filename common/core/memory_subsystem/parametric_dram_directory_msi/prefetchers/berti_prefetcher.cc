#include "berti_prefetcher.h"
#include "simulator.h"
#include "config.hpp"
#include "stats.h"

#include <cstdlib>
#include <cstring>

BertiPrefetcher::BertiPrefetcher(String configName, core_id_t core_id)
   : m_core_id(core_id)
   , m_current_cycle(0)
   , m_prev_requests_head(0)
{
   for (UInt32 i = 0; i < CURRENT_PAGES_ENTRIES; i++)
      m_current_pages[i].lru = i;
   for (UInt32 i = 0; i < RECORD_PAGES_ENTRIES; i++)
      m_record_pages[i].lru = i;
   for (UInt32 i = 0; i < IP_TABLE_ENTRIES; i++)
      m_ip_table[i] = RECORD_PAGES_ENTRIES;

   memset(&stats, 0, sizeof(stats));
   registerStatsMetric("berti", m_core_id, "pref_called", &stats.pref_called);
   registerStatsMetric("berti", m_core_id, "current_pages_hits", &stats.current_pages_hits);
   registerStatsMetric("berti", m_core_id, "record_pages_hits", &stats.record_pages_hits);
   registerStatsMetric("berti", m_core_id, "prefetches_issued", &stats.prefetches_issued);
   registerStatsMetric("berti", m_core_id, "berti_predictions", &stats.berti_predictions);
}

BertiPrefetcher::~BertiPrefetcher()
{
}

std::vector<IntPtr>
BertiPrefetcher::getNextAddress(IntPtr current_address, core_id_t core_id, Core::mem_op_t mem_op_type, bool cache_hit, bool prefetch_hit, IntPtr eip)
{
   std::vector<IntPtr> pref_addr;
   if (mem_op_type == Core::WRITE)
      return pref_addr;

   stats.pref_called++;
   m_current_cycle++;

   UInt64 line_addr = current_address >> LOG2_BLOCK_SIZE;
   UInt64 page_addr = line_addr >> PAGE_BLOCKS_BITS;
   UInt64 offset = line_addr & PAGE_OFFSET_MASK;
   const UInt64 ip_index = eip & (IP_TABLE_ENTRIES - 1);

   UInt64 index = findCurrentPage(page_addr);

   // A line already demanded on a page in flight teaches nothing new
   if (index < CURRENT_PAGES_ENTRIES && requested(index, offset))
      return pref_addr;

   if (index < CURRENT_PAGES_ENTRIES)
   {
      stats.current_pages_hits++;
      UInt64 first_ip = demandCurrentPage(index, offset);

      // A hit means an earlier request covered this line in time: the
      // deltas from those requests are the candidate bertis
      if (cache_hit)
      {
         SInt32 b[MAX_BERTI_CANDIDATES];
         bertiCandidates(index, offset, b);
         for (UInt32 i = 0; i < MAX_BERTI_CANDIDATES && b[i] != 0; i++)
            if ((UInt32)std::abs(b[i]) < PAGE_BLOCKS)
               addBerti(index, b[i]);
      }

      // Every IP that touches the page shares the trigger IP's record
      if (first_ip != ip_index)
         m_ip_table[ip_index] = m_ip_table[first_ip];
   }
   else
   {
      // A new page: retire the LRU one into the record tables first
      UInt64 victim = lruCurrentPage();
      resetPrevRequests(victim);
      recordCurrentPage(victim);

      index = victim;
      addCurrentPage(index, page_addr, ip_index, offset);

      UInt64 index_record = findRecordPage(page_addr, offset);
      if (m_ip_table[ip_index] == RECORD_PAGES_ENTRIES)
         m_ip_table[ip_index] = (index_record == RECORD_PAGES_ENTRIES) ? lruRecordPage() : index_record;
   }

   addPrevRequest(index, offset, m_current_cycle);

   // Predict: the record for this page and first offset, else this IP's
   // record for the same first offset, else the berti learned on the page
   // itself, else any record for the page, else any record for the IP.
   UInt64 u_vector = 0;
   UInt64 first_offset = m_current_pages[index].first_offset;
   SInt32 berti = 0;
   bool recorded = false;
   UInt32 berti_confidence = 0;

   UInt64 ip_pointer = m_ip_table[ip_index];
   UInt64 pgo_pointer = findRecordPage(page_addr, first_offset);
   UInt64 pg_pointer = findRecordPage(page_addr);
   SInt32 current_berti = bestBerti(index, berti_confidence);

   if (pgo_pointer != RECORD_PAGES_ENTRIES
       && (m_record_pages[pgo_pointer].u_vector | m_current_pages[index].u_vector) == m_record_pages[pgo_pointer].u_vector)
   {
      u_vector = m_record_pages[pgo_pointer].u_vector;
      berti = m_record_pages[pgo_pointer].berti;
      recorded = true;
      stats.record_pages_hits++;
   }
   else if (ip_pointer < RECORD_PAGES_ENTRIES
            && m_record_pages[ip_pointer].first_offset == first_offset
            && (m_record_pages[ip_pointer].u_vector | m_current_pages[index].u_vector) == m_record_pages[ip_pointer].u_vector)
   {
      u_vector = m_record_pages[ip_pointer].u_vector;
      berti = m_record_pages[ip_pointer].berti;
      recorded = true;
      stats.record_pages_hits++;
   }
   else if (current_berti != 0 && berti_confidence >= BERTI_CTR_CONFIDENCE)
   {
      u_vector = m_current_pages[index].u_vector;
      berti = current_berti;
   }
   else if (pg_pointer != RECORD_PAGES_ENTRIES)
   {
      u_vector = m_record_pages[pg_pointer].u_vector;
      berti = m_record_pages[pg_pointer].berti;
      recorded = true;
   }
   else if (ip_pointer < RECORD_PAGES_ENTRIES && m_record_pages[ip_pointer].u_vector)
   {
      u_vector = m_record_pages[ip_pointer].u_vector;
      berti = m_record_pages[ip_pointer].berti;
      recorded = true;
   }

   if (berti == 0)
      return pref_addr;

   stats.berti_predictions++;

   // The berti itself, within the page and not already demanded
   SInt64 pf_line = (SInt64)line_addr + berti;
   if (pf_line >= 0)
   {
      UInt64 pf_page = (UInt64)pf_line >> PAGE_BLOCKS_BITS;
      UInt64 pf_offset = (UInt64)pf_line & PAGE_OFFSET_MASK;
      if (pf_page == page_addr && !requested(index, pf_offset))
      {
         pref_addr.push_back((IntPtr)pf_line << LOG2_BLOCK_SIZE);
         stats.prefetches_issued++;
      }
   }

   // On the first access to a recorded page, burst the lines used last
   // time that lie between here and the berti
   if (first_offset == offset && recorded)
   {
      UInt32 bursts = 0;
      if (berti > 0)
      {
         for (UInt64 i = offset + 1; i < offset + (UInt64)berti && bursts < MAX_BURST_PREFETCHES; i++)
         {
            if (i >= PAGE_BLOCKS)
               break;
            if (((UInt64)1 << i) & u_vector && !requested(index, i))
            {
               pref_addr.push_back((IntPtr)(((page_addr << PAGE_BLOCKS_BITS) | i) << LOG2_BLOCK_SIZE));
               stats.prefetches_issued++;
               bursts++;
            }
         }
      }
      else
      {
         for (SInt64 i = (SInt64)offset - 1; i > (SInt64)offset + berti && bursts < MAX_BURST_PREFETCHES; i--)
         {
            if (i < 0)
               break;
            if (((UInt64)1 << i) & u_vector && !requested(index, (UInt64)i))
            {
               pref_addr.push_back((IntPtr)(((page_addr << PAGE_BLOCKS_BITS) | (UInt64)i) << LOG2_BLOCK_SIZE));
               stats.prefetches_issued++;
               bursts++;
            }
         }
      }
   }

   return pref_addr;
}

// Current pages

UInt64 BertiPrefetcher::findCurrentPage(UInt64 page_addr)
{
   for (UInt32 i = 0; i < CURRENT_PAGES_ENTRIES; i++)
      if (m_current_pages[i].page_addr == page_addr && m_current_pages[i].u_vector != 0)
         return i;
   return CURRENT_PAGES_ENTRIES;
}

void BertiPrefetcher::touchCurrentPage(UInt64 index)
{
   for (UInt32 i = 0; i < CURRENT_PAGES_ENTRIES; i++)
      if (m_current_pages[i].lru < m_current_pages[index].lru)
         m_current_pages[i].lru++;
   m_current_pages[index].lru = 0;
}

UInt64 BertiPrefetcher::lruCurrentPage()
{
   UInt64 lru = CURRENT_PAGES_ENTRIES;
   for (UInt32 i = 0; i < CURRENT_PAGES_ENTRIES; i++)
   {
      m_current_pages[i].lru++;
      if (m_current_pages[i].lru == CURRENT_PAGES_ENTRIES)
      {
         m_current_pages[i].lru = 0;
         lru = i;
      }
   }
   return lru;
}

SInt32 BertiPrefetcher::bestBerti(UInt64 index, UInt32 &confidence)
{
   UInt32 max_score = 0;
   SInt32 best = 0;
   confidence = 0;
   for (UInt32 i = 0; i < NUM_BERTI; i++)
   {
      if (m_current_pages[index].berti_ctr[i] > max_score)
      {
         best = m_current_pages[index].berti[i];
         max_score = m_current_pages[index].berti_ctr[i];
         confidence = max_score;
      }
   }
   return best;
}

void BertiPrefetcher::addCurrentPage(UInt64 index, UInt64 page_addr, UInt64 ip, UInt64 offset)
{
   m_current_pages[index].page_addr = page_addr;
   m_current_pages[index].ip = ip;
   m_current_pages[index].u_vector = (UInt64)1 << offset;
   m_current_pages[index].first_offset = offset;
   for (UInt32 i = 0; i < NUM_BERTI; i++)
   {
      m_current_pages[index].berti[i] = 0;
      m_current_pages[index].berti_ctr[i] = 0;
   }
}

UInt64 BertiPrefetcher::demandCurrentPage(UInt64 index, UInt64 offset)
{
   m_current_pages[index].u_vector |= (UInt64)1 << offset;
   touchCurrentPage(index);
   return m_current_pages[index].ip;
}

void BertiPrefetcher::addBerti(UInt64 index, SInt32 b)
{
   if (b == 0)
      return;
   for (UInt32 i = 0; i < NUM_BERTI; i++)
   {
      if (m_current_pages[index].berti_ctr[i] == 0)
      {
         m_current_pages[index].berti[i] = b;
         m_current_pages[index].berti_ctr[i] = 1;
         break;
      }
      else if (m_current_pages[index].berti[i] == b)
      {
         m_current_pages[index].berti_ctr[i]++;
         break;
      }
   }
   touchCurrentPage(index);
}

bool BertiPrefetcher::requested(UInt64 index, UInt64 offset) const
{
   return m_current_pages[index].u_vector & ((UInt64)1 << offset);
}

// Previous requests

UInt64 BertiPrefetcher::findPrevRequest(UInt64 pointer, UInt64 offset)
{
   for (UInt32 i = 0; i < PREV_REQUESTS_ENTRIES; i++)
      if (m_prev_requests[i].page_index == pointer && m_prev_requests[i].offset == offset)
         return i;
   return PREV_REQUESTS_ENTRIES;
}

void BertiPrefetcher::addPrevRequest(UInt64 pointer, UInt64 offset, UInt64 cycle)
{
   if (findPrevRequest(pointer, offset) != PREV_REQUESTS_ENTRIES)
      return;
   m_prev_requests[m_prev_requests_head].page_index = pointer;
   m_prev_requests[m_prev_requests_head].offset = offset;
   m_prev_requests[m_prev_requests_head].time = cycle;
   m_prev_requests_head = (m_prev_requests_head + 1) & (PREV_REQUESTS_ENTRIES - 1);
}

void BertiPrefetcher::resetPrevRequests(UInt64 pointer)
{
   for (UInt32 i = 0; i < PREV_REQUESTS_ENTRIES; i++)
      if (m_prev_requests[i].page_index == pointer)
         m_prev_requests[i].page_index = CURRENT_PAGES_ENTRIES;
}

void BertiPrefetcher::bertiCandidates(UInt64 pointer, UInt64 offset, SInt32 *b)
{
   UInt32 count = 0;
   for (UInt32 i = 0; i < MAX_BERTI_CANDIDATES; i++)
      b[i] = 0;
   for (UInt32 i = 0; i < PREV_REQUESTS_ENTRIES && count < MAX_BERTI_CANDIDATES; i++)
   {
      if (m_prev_requests[i].page_index == pointer && m_prev_requests[i].offset != offset)
      {
         SInt32 s = stride(m_prev_requests[i].offset, offset);
         if (s != 0 && (UInt32)std::abs(s) < PAGE_BLOCKS)
            b[count++] = s;
      }
   }
}

// Record pages

UInt64 BertiPrefetcher::lruRecordPage()
{
   UInt64 max_lru = 0;
   UInt64 lru_index = 0;
   for (UInt32 i = 0; i < RECORD_PAGES_ENTRIES; i++)
   {
      if (m_record_pages[i].lru > max_lru)
      {
         max_lru = m_record_pages[i].lru;
         lru_index = i;
      }
   }
   return lru_index;
}

void BertiPrefetcher::touchRecordPage(UInt64 index)
{
   for (UInt32 i = 0; i < RECORD_PAGES_ENTRIES; i++)
      if (m_record_pages[i].lru < m_record_pages[index].lru)
         m_record_pages[i].lru++;
   m_record_pages[index].lru = 0;
}

void BertiPrefetcher::addRecordPage(UInt64 index, UInt64 page_addr, UInt64 vector, UInt64 first_offset, SInt32 berti)
{
   m_record_pages[index].page_addr = page_addr;
   m_record_pages[index].u_vector = vector;
   m_record_pages[index].first_offset = first_offset;
   m_record_pages[index].berti = berti;
   touchRecordPage(index);
}

UInt64 BertiPrefetcher::findRecordPage(UInt64 page_addr, UInt64 first_offset)
{
   for (UInt32 i = 0; i < RECORD_PAGES_ENTRIES; i++)
   {
      if (m_record_pages[i].page_addr == page_addr && m_record_pages[i].first_offset == first_offset && m_record_pages[i].u_vector != 0)
      {
         touchRecordPage(i);
         return i;
      }
   }
   return RECORD_PAGES_ENTRIES;
}

UInt64 BertiPrefetcher::findRecordPage(UInt64 page_addr)
{
   for (UInt32 i = 0; i < RECORD_PAGES_ENTRIES; i++)
   {
      if (m_record_pages[i].page_addr == page_addr && m_record_pages[i].u_vector != 0)
      {
         touchRecordPage(i);
         return i;
      }
   }
   return RECORD_PAGES_ENTRIES;
}

void BertiPrefetcher::recordCurrentPage(UInt64 index)
{
   if (m_current_pages[index].u_vector == 0)
      return;
   UInt32 confidence = 0;
   SInt32 berti = bestBerti(index, confidence);
   addRecordPage(lruRecordPage(), m_current_pages[index].page_addr, m_current_pages[index].u_vector,
                 m_current_pages[index].first_offset, berti);
}

SInt32 BertiPrefetcher::stride(UInt64 prev_offset, UInt64 current_offset)
{
   return current_offset > prev_offset ? (SInt32)(current_offset - prev_offset) : -(SInt32)(prev_offset - current_offset);
}
