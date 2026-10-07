#ifndef SMS_PREFETCHER_H
#define SMS_PREFETCHER_H

// SMS, Spatial Memory Streaming (Somogyi et al., ISCA 2006), ported from
// Virtuoso's port of ChampSim's implementation.  The first access to a
// spatial region goes into a filter table; a second access moves the region
// to an accumulation table that records which lines of it get touched; when
// the region is evicted from there its bit pattern is stored in a pattern
// history table keyed by (trigger PC, trigger offset).  The next time that
// trigger fires on a fresh region, the stored pattern is replayed as
// prefetches, pref_degree per access through a prefetch buffer.
//
// Statistics are registered under "sms"; prefetches_issued is the number of
// addresses handed to the cache controller.

#include "prefetcher.h"

#include <vector>
#include <deque>
#include <bitset>

class SMSPrefetcher : public Prefetcher
{
   public:
      SMSPrefetcher(String configName, core_id_t core_id);
      ~SMSPrefetcher();

      std::vector<IntPtr> getNextAddress(IntPtr current_address, core_id_t core_id, Core::mem_op_t mem_op_type, bool cache_hit, bool prefetch_hit, IntPtr eip);

   private:
      // A region is at most 64 lines (4 KB): the pattern is a 64-bit map
      static const UInt32 BITMAP_MAX_SIZE = 64;
      typedef std::bitset<BITMAP_MAX_SIZE> Bitmap;

      static const UInt32 LOG2_BLOCK_SIZE = 6;

      core_id_t m_core_id;

      // perf_model/<cache>/prefetcher/sms/*
      UInt32 m_at_size;            // accumulation table entries
      UInt32 m_ft_size;            // filter table entries
      UInt32 m_pht_size;           // pattern history table entries
      UInt32 m_pht_assoc;          // ... and its associativity
      UInt32 m_pref_degree;        // prefetches issued per access
      UInt32 m_region_size;        // spatial region, in bytes
      UInt32 m_region_size_log;    // log2 of it
      UInt32 m_pref_buffer_size;   // prefetches queued between accesses

      struct FTEntry
      {
         UInt64 page;
         UInt64 pc;
         UInt32 trigger_offset;
         FTEntry() : page(0), pc(0), trigger_offset(0) {}
      };

      struct ATEntry
      {
         UInt64 page;
         UInt64 pc;
         UInt32 trigger_offset;
         Bitmap pattern;
         UInt32 age;
         ATEntry() : page(0), pc(0), trigger_offset(0), pattern(0), age(0) {}
      };

      struct PHTEntry
      {
         UInt64 signature;
         Bitmap pattern;
         UInt32 age;
         PHTEntry() : signature(0), pattern(0), age(0) {}
      };

      std::deque<FTEntry> m_filter_table;
      std::deque<ATEntry> m_acc_table;
      std::vector<std::deque<PHTEntry> > m_pht;
      std::deque<IntPtr> m_pref_buffer;

      std::deque<FTEntry>::iterator findFilter(UInt64 page);
      void insertFilter(UInt64 pc, UInt64 page, UInt32 offset);

      std::deque<ATEntry>::iterator findAcc(UInt64 page);
      void insertAcc(const FTEntry &ft, UInt32 offset);
      void evictAcc(std::deque<ATEntry>::iterator victim);
      void touchAcc(std::deque<ATEntry>::iterator current);

      void insertPHT(const ATEntry &at);
      std::deque<PHTEntry>::iterator findPHT(UInt64 signature, UInt32 &set);
      void touchPHT(UInt32 set, std::deque<PHTEntry>::iterator current);

      UInt64 signature(UInt64 pc, UInt32 offset) const;
      void generatePrefetches(UInt64 pc, UInt64 page, UInt32 offset);
      std::vector<IntPtr> issuePrefetches();

      struct
      {
         UInt64 pref_called;
         UInt64 ft_hits;
         UInt64 at_hits;
         UInt64 pht_hits;
         UInt64 prefetches_generated;
         UInt64 prefetches_issued;
      } stats;
};

#endif // SMS_PREFETCHER_H
