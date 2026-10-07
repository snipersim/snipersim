#ifndef IP_STRIDE_PREFETCHER_H
#define IP_STRIDE_PREFETCHER_H

// IP-stride data prefetcher (Baer and Chen, 1991, DOI 10.1145/125826.125932),
// ported from Virtuoso.  A reference prediction table (RPT) keyed by the
// instruction pointer learns the stride between a load's consecutive cache
// lines; once an entry is STEADY it prefetches num_prefetches lines ahead of
// the access, lookahead strides out.  Writes are ignored.
//
// Statistics are registered under "ip_stride"; gen_prefetch is the number of
// prefetch addresses handed to the cache controller.

#include "prefetcher.h"

#include <vector>

class IPStridePrefetcher : public Prefetcher
{
   public:
      enum rpt_state_t
      {
         INVALID = 0,
         INIT,
         TRANSIENT,
         STEADY,
         NO_PRED,
      };

      struct RPTEntry
      {
         bool valid;
         IntPtr eip;
         IntPtr prev_offset;
         SInt32 stride;
         rpt_state_t state;
         UInt32 lru;

         RPTEntry() : valid(false), eip(0), prev_offset(0), stride(0), state(INVALID), lru(0) {}
      };

      IPStridePrefetcher(String configName, core_id_t core_id);
      ~IPStridePrefetcher();

      std::vector<IntPtr> getNextAddress(IntPtr current_address, core_id_t core_id, Core::mem_op_t mem_op_type, bool cache_hit, bool prefetch_hit, IntPtr eip);

   private:
      core_id_t m_core_id;
      bool m_allocate_on_miss;      // allocate an RPT entry only on a cache miss
      UInt32 m_num_prefetches;      // lines prefetched per STEADY access
      UInt32 m_num_streams;         // RPT entries
      bool m_stop_at_page;          // do not cross the 4 KB page
      UInt32 m_lookahead;           // strides between the access and the first prefetch
      std::vector<RPTEntry> m_rpt;

      struct
      {
         UInt64 pref_called;
         UInt64 rpt_hit;
         UInt64 non_zero_curr_stride;
         UInt64 curr_stride_correct;
         UInt64 steady;
         UInt64 rpt_replacement;
         UInt64 gen_prefetch;
      } stats;

      SInt32 find(IntPtr eip);
      void updateAge(SInt32 current);
      SInt32 findReplacement();
      std::vector<IntPtr> generatePrefetchAddresses(const RPTEntry &entry, IntPtr current_page, IntPtr current_offset);
};

#endif // IP_STRIDE_PREFETCHER_H
