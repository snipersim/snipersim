#ifndef STREAM_PREFETCHER_H
#define STREAM_PREFETCHER_H

// Stream (next-line direction) data prefetcher, ported from Virtuoso.  A
// table of per-page streams tracks the direction of consecutive accesses
// within a 4 KB page with a saturating confidence counter; once the
// confidence reaches conf_thresh it prefetches num_prefetches lines in that
// direction, prefetch_front lines ahead.  Never crosses the page.  Writes are
// ignored.
//
// Statistics are registered under "streamer"; gen_prefetch is the number of
// prefetch addresses handed to the cache controller.

#include "prefetcher.h"

#include <vector>

class Streamer : public Prefetcher
{
   public:
      struct StreamEntry
      {
         bool valid;
         IntPtr page;
         IntPtr last_offset;
         SInt8 dir;
         UInt8 conf;
         UInt32 lru;

         StreamEntry() : valid(false), page(0), last_offset(0), dir(0), conf(0), lru(0) {}
      };

      Streamer(String configName, core_id_t core_id);
      ~Streamer();

      std::vector<IntPtr> getNextAddress(IntPtr current_address, core_id_t core_id, Core::mem_op_t mem_op_type, bool cache_hit, bool prefetch_hit, IntPtr eip);

   private:
      core_id_t m_core_id;
      bool m_allocate_on_miss;      // allocate a stream only on a cache miss
      UInt32 m_num_prefetches;      // lines prefetched per confident access
      UInt32 m_num_streams;         // stream table entries
      UInt32 m_prefetch_front;      // lines between the access and the first prefetch
      UInt8 m_max_conf;             // confidence saturates here
      UInt8 m_conf_thresh;          // confidence needed to prefetch
      std::vector<StreamEntry> m_stream_table;

      struct
      {
         UInt64 pref_called;
         UInt64 table_hit;
         UInt64 table_replacement;
         UInt64 incr_conf;
         UInt64 decr_conf;
         UInt64 steady;
         UInt64 gen_prefetch;
      } stats;

      SInt32 find(IntPtr page);
      void updateAge(SInt32 current);
      SInt32 findReplacement();
      std::vector<IntPtr> generatePrefetchAddresses(const StreamEntry &entry, IntPtr current_page, IntPtr current_offset);
};

#endif // STREAM_PREFETCHER_H
