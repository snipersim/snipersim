#ifndef IPCP_PREFETCHER_H
#define IPCP_PREFETCHER_H

// IPCP, the Instruction Pointer Classifier Prefetcher (Pakalapati and
// Panda, ISCA 2020), ported from Virtuoso's port of ChampSim's
// implementation.  Each load IP is classified from its recent line deltas
// as a stream (the last 16 lines around it were touched in order), a
// constant stride, or a complex stride (a signature of recent deltas with
// a learned next delta), and prefetched accordingly, prefetch_degree lines
// at a time (twice that for a stream); an IP that fits no class gets a
// next-line prefetch while the miss rate is low enough (spec_nl_threshold
// misses per thousand accesses).
//
// Statistics are registered under "ipcp"; prefetches_issued is the number
// of addresses handed to the cache controller.

#include "prefetcher.h"

#include <vector>

class IPCPPrefetcher : public Prefetcher
{
   public:
      IPCPPrefetcher(String configName, core_id_t core_id);
      ~IPCPPrefetcher();

      std::vector<IntPtr> getNextAddress(IntPtr current_address, core_id_t core_id, Core::mem_op_t mem_op_type, bool cache_hit, bool prefetch_hit, IntPtr eip);

   private:
      core_id_t m_core_id;

      static const UInt32 LOG2_PAGE_SIZE = 12;
      static const UInt32 LOG2_BLOCK_SIZE = 6;
      static const UInt32 NUM_IP_TABLE_ENTRIES = 1024;
      static const UInt32 NUM_GHB_ENTRIES = 16;
      static const UInt32 NUM_IP_INDEX_BITS = 10;
      static const UInt32 NUM_IP_TAG_BITS = 6;
      static const UInt32 DPT_SIZE = 4096;   // one per 12-bit signature
      static const UInt32 MISSES_PER_SAMPLE = 256;

      // perf_model/<cache>/prefetcher/ipcp/*
      UInt32 m_prefetch_degree;
      UInt32 m_spec_nl_threshold;

      struct IPTableEntry
      {
         UInt64 ip_tag;
         UInt64 last_page;
         UInt64 last_cl_offset;
         SInt64 last_stride;
         bool ip_valid;
         SInt32 conf;           // constant-stride confidence
         UInt16 signature;      // complex-stride signature
         bool str_dir;          // stream direction, true = ascending
         bool str_valid;
         bool str_strength;

         IPTableEntry() : ip_tag(0), last_page(0), last_cl_offset(0), last_stride(0), ip_valid(false), conf(0),
                          signature(0), str_dir(false), str_valid(false), str_strength(false) {}
      };

      struct DPTEntry
      {
         SInt32 delta;
         SInt32 conf;
         DPTEntry() : delta(0), conf(0) {}
      };

      IPTableEntry m_ip_table[NUM_IP_TABLE_ENTRIES];
      DPTEntry m_dpt[DPT_SIZE];
      UInt64 m_ghb[NUM_GHB_ENTRIES];

      // Speculative next-line: on while the miss rate is low
      UInt64 m_num_misses;
      UInt64 m_prev_sample;
      bool m_spec_nl;

      static UInt16 updateSignature(UInt16 old_sig, SInt32 delta);
      static SInt32 updateConf(SInt32 stride, SInt32 pred_stride, SInt32 conf);
      void checkForStream(UInt32 index, UInt64 cl_addr);
      void updateGHB(UInt64 cl_addr);

      struct
      {
         UInt64 pref_called;
         UInt64 stream_prefetches;
         UInt64 cs_prefetches;
         UInt64 cplx_prefetches;
         UInt64 nl_prefetches;
         UInt64 prefetches_issued;
      } stats;
};

#endif // IPCP_PREFETCHER_H
