#ifndef BERTI_PREFETCHER_H
#define BERTI_PREFETCHER_H

// Berti data prefetcher (Navarro-Torres et al., MICRO 2022), ported from
// Virtuoso's port of ChampSim's implementation.  For each page in flight it
// records which lines were demanded and when, derives the "berti" -- the
// line delta that would have been timely -- from the previous requests, and
// prefetches that delta ahead of each demand; the pattern of a page that
// leaves the current-pages table is recorded by page and by trigger IP so
// that the next visit starts with a learned berti and a burst of the lines
// used last time.  This is the data-side Berti: the TLB one is
// BertiTLBPrefetcher (tlb_berti.*).
//
// The timing source is the access count, as upstream: a cycle counter is
// not available at this interface.
//
// Statistics are registered under "berti"; prefetches_issued is the number
// of addresses handed to the cache controller.

#include "prefetcher.h"

#include <vector>

class BertiPrefetcher : public Prefetcher
{
   public:
      BertiPrefetcher(String configName, core_id_t core_id);
      ~BertiPrefetcher();

      std::vector<IntPtr> getNextAddress(IntPtr current_address, core_id_t core_id, Core::mem_op_t mem_op_type, bool cache_hit, bool prefetch_hit, IntPtr eip);

   private:
      core_id_t m_core_id;
      UInt64 m_current_cycle;

      static const UInt32 LOG2_PAGE_SIZE = 12;
      static const UInt32 LOG2_BLOCK_SIZE = 6;
      static const UInt32 PAGE_BLOCKS_BITS = LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE;
      static const UInt32 PAGE_BLOCKS = 1 << PAGE_BLOCKS_BITS;
      static const UInt32 PAGE_OFFSET_MASK = PAGE_BLOCKS - 1;
      static const UInt32 MAX_BURST_PREFETCHES = 3;
      static const UInt32 BERTI_CTR_CONFIDENCE = 2;
      static const UInt32 MAX_BERTI_CANDIDATES = 7;

      static const UInt32 CURRENT_PAGES_ENTRIES = 63;
      static const UInt32 NUM_BERTI = 10;
      static const UInt32 PREV_REQUESTS_ENTRIES = 1024;
      static const UInt32 RECORD_PAGES_ENTRIES = 1024;
      static const UInt32 IP_TABLE_ENTRIES = 1024;

      struct CurrentPageEntry
      {
         UInt64 page_addr;
         UInt64 ip;
         UInt64 u_vector;              // lines demanded so far
         UInt64 first_offset;
         SInt32 berti[NUM_BERTI];      // candidate deltas
         UInt32 berti_ctr[NUM_BERTI];  // and their confidence
         UInt64 lru;

         CurrentPageEntry() : page_addr(0), ip(0), u_vector(0), first_offset(0), lru(0)
         {
            for (UInt32 i = 0; i < NUM_BERTI; i++)
            {
               berti[i] = 0;
               berti_ctr[i] = 0;
            }
         }
      };

      struct PrevRequestEntry
      {
         UInt64 page_index;
         UInt64 offset;
         UInt64 time;
         PrevRequestEntry() : page_index(CURRENT_PAGES_ENTRIES), offset(0), time(0) {}
      };

      struct RecordPageEntry
      {
         UInt64 page_addr;
         UInt64 u_vector;
         UInt64 first_offset;
         SInt32 berti;
         UInt64 lru;
         RecordPageEntry() : page_addr(0), u_vector(0), first_offset(0), berti(0), lru(0) {}
      };

      CurrentPageEntry m_current_pages[CURRENT_PAGES_ENTRIES];
      PrevRequestEntry m_prev_requests[PREV_REQUESTS_ENTRIES];
      UInt64 m_prev_requests_head;
      RecordPageEntry m_record_pages[RECORD_PAGES_ENTRIES];
      UInt64 m_ip_table[IP_TABLE_ENTRIES];   // IP -> record page, RECORD_PAGES_ENTRIES when none

      // Current pages
      UInt64 findCurrentPage(UInt64 page_addr);
      void touchCurrentPage(UInt64 index);
      UInt64 lruCurrentPage();
      SInt32 bestBerti(UInt64 index, UInt32 &confidence);
      void addCurrentPage(UInt64 index, UInt64 page_addr, UInt64 ip, UInt64 offset);
      UInt64 demandCurrentPage(UInt64 index, UInt64 offset);
      void addBerti(UInt64 index, SInt32 b);
      bool requested(UInt64 index, UInt64 offset) const;

      // Previous requests
      UInt64 findPrevRequest(UInt64 pointer, UInt64 offset);
      void addPrevRequest(UInt64 pointer, UInt64 offset, UInt64 cycle);
      void resetPrevRequests(UInt64 pointer);
      void bertiCandidates(UInt64 pointer, UInt64 offset, SInt32 *b);

      // Record pages
      UInt64 lruRecordPage();
      void touchRecordPage(UInt64 index);
      void addRecordPage(UInt64 index, UInt64 page_addr, UInt64 vector, UInt64 first_offset, SInt32 berti);
      UInt64 findRecordPage(UInt64 page_addr, UInt64 first_offset);
      UInt64 findRecordPage(UInt64 page_addr);
      void recordCurrentPage(UInt64 index);

      static SInt32 stride(UInt64 prev_offset, UInt64 current_offset);

      struct
      {
         UInt64 pref_called;
         UInt64 current_pages_hits;
         UInt64 record_pages_hits;
         UInt64 prefetches_issued;
         UInt64 berti_predictions;
      } stats;
};

#endif // BERTI_PREFETCHER_H
