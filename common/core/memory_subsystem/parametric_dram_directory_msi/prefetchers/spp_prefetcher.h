#ifndef SPP_PREFETCHER_H
#define SPP_PREFETCHER_H

// SPP, the Signature Path Prefetcher (Kim et al., MICRO 2016), ported from
// Virtuoso's port of ChampSim's spp_dev.  A signature table hashes the
// recent delta history of each page into a signature; a pattern table maps
// a signature to the deltas that followed it and their confidence; the
// prefetcher walks that path -- each predicted delta rehashes into the next
// signature (lookahead) -- issuing every delta whose compounded confidence
// clears PF_THRESHOLD and stays inside the page.  A quotient filter drops
// duplicates and feeds the global accuracy counters; deltas that would leave
// the page are remembered in the GHR.
//
// Statistics are registered under "spp"; prefetches_issued is the number of
// addresses handed to the cache controller.

#include "prefetcher.h"

#include <vector>

class SPPPrefetcher : public Prefetcher
{
   public:
      SPPPrefetcher(String configName, core_id_t core_id);
      ~SPPPrefetcher();

      std::vector<IntPtr> getNextAddress(IntPtr current_address, core_id_t core_id, Core::mem_op_t mem_op_type, bool cache_hit, bool prefetch_hit, IntPtr eip);

   private:
      core_id_t m_core_id;

      // The three optional knobs (perf_model/<cache>/prefetcher/spp/*)
      bool m_lookahead_on;
      bool m_filter_on;
      bool m_ghr_on;

      // Signature table
      static const UInt32 ST_SET = 1;
      static const UInt32 ST_WAY = 256;
      static const UInt32 ST_TAG_BIT = 16;
      static const UInt32 SIG_SHIFT = 3;
      static const UInt32 SIG_BIT = 12;
      static const UInt32 SIG_MASK = (1 << SIG_BIT) - 1;
      static const UInt32 SIG_DELTA_BIT = 7;

      // Pattern table
      static const UInt32 PT_SET = 512;
      static const UInt32 PT_WAY = 4;
      static const UInt32 C_SIG_BIT = 4;
      static const UInt32 C_DELTA_BIT = 4;
      static const UInt32 C_SIG_MAX = (1 << C_SIG_BIT) - 1;
      static const UInt32 C_DELTA_MAX = (1 << C_DELTA_BIT) - 1;

      // Prefetch filter
      static const UInt32 QUOTIENT_BIT = 10;
      static const UInt32 REMAINDER_BIT = 6;
      static const UInt32 FILTER_SET = 1 << QUOTIENT_BIT;
      static const UInt32 FILL_THRESHOLD = 90;
      static const UInt32 PF_THRESHOLD = 25;

      // Global history register
      static const UInt32 GLOBAL_COUNTER_BIT = 10;
      static const UInt32 GLOBAL_COUNTER_MAX = (1 << GLOBAL_COUNTER_BIT) - 1;
      static const UInt32 MAX_GHR_ENTRY = 8;

      static const UInt32 LOG2_PAGE_SIZE = 12;
      static const UInt32 LOG2_BLOCK_SIZE = 6;
      static const UInt32 BLOCKS_PER_PAGE = 1 << (LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE);

      // The prefetch queue is bounded like the MSHR it would occupy
      static const UInt32 PF_QUEUE = 16;

      enum FilterRequest { SPP_L2C_PREFETCH, SPP_LLC_PREFETCH, L2C_DEMAND, L2C_EVICT };

      struct SignatureTable
      {
         bool valid[ST_SET][ST_WAY];
         UInt64 tag[ST_SET][ST_WAY];
         UInt32 last_offset[ST_SET][ST_WAY];
         UInt32 sig[ST_SET][ST_WAY];
         UInt32 lru[ST_SET][ST_WAY];

         SignatureTable();
         bool readAndUpdateSig(IntPtr addr, UInt32 &last_sig, UInt32 &curr_sig, SInt32 &delta);   // true on a tag match
      } ST;

      struct PatternTable
      {
         SInt32 delta[PT_SET][PT_WAY];
         UInt32 c_delta[PT_SET][PT_WAY];
         UInt32 c_sig[PT_SET];

         PatternTable();
         void updatePattern(UInt32 last_sig, SInt32 curr_delta);
         void readPattern(UInt32 curr_sig, std::vector<SInt32> &prefetch_delta, std::vector<UInt32> &confidence_q,
                          UInt32 &lookahead_way, UInt32 &lookahead_conf, UInt32 &pf_q_tail, UInt32 &depth);
      } PT;

      struct PrefetchFilter
      {
         UInt64 remainder_tag[FILTER_SET];
         bool valid[FILTER_SET];
         bool useful[FILTER_SET];

         PrefetchFilter();
         bool check(IntPtr pf_addr, FilterRequest filter_request, UInt32 &pf_useful, UInt32 &pf_issued);
      } FILTER;

      struct GlobalRegister
      {
         UInt32 pf_useful;
         UInt32 pf_issued;
         UInt32 global_accuracy;

         bool valid[MAX_GHR_ENTRY];
         UInt32 sig[MAX_GHR_ENTRY];
         UInt32 confidence[MAX_GHR_ENTRY];
         UInt32 offset[MAX_GHR_ENTRY];
         SInt32 delta[MAX_GHR_ENTRY];

         GlobalRegister();
         void updateEntry(UInt32 pf_sig, UInt32 pf_confidence, UInt32 pf_offset, SInt32 pf_delta);
      } GHR;

      static UInt64 hash(UInt64 key);

      struct
      {
         UInt64 pref_called;
         UInt64 prefetches_issued;
         UInt64 st_hits;
         UInt64 st_misses;
         UInt64 filter_hits;
      } stats;
};

#endif // SPP_PREFETCHER_H
