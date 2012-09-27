#ifndef __GHB_PREFETCHER_H
#define __GHB_PREFETCHER_H

#include "prefetcher.h"

class GhbPrefetcher : public Prefetcher
{
   public:
      GhbPrefetcher(String configName, core_id_t core_id);
      std::vector<IntPtr> getNextAddress(IntPtr currentAddress);

      ~GhbPrefetcher();

   private:
      static const SInt64 INVALID_DELTA = INT64_MAX;
      static const UInt32 INVALID_INDEX = UINT32_MAX;

      struct GHBEntry
      {
         UInt32 nextIndex; //index of the next entry belonging to the same list
         SInt64 delta; //delta between last address and current address
         UInt32 generation;
         GHBEntry() : nextIndex(INVALID_INDEX), delta(INVALID_DELTA), generation(0) {}
      };

      struct TableEntry
      {
         UInt32 ghbIndex;
         SInt64 delta;
         UInt32 generation;
         TableEntry() : ghbIndex(INVALID_INDEX), delta(INVALID_DELTA), generation(0) {}
      };

      UInt32 m_prefetchWidth;
      UInt32 m_prefetchDepth;

      IntPtr m_lastAddress;

      //circular global history buffer
      UInt32 m_ghbSize;
      UInt32 m_ghbHead;
      UInt32 m_generation;
      std::vector<GHBEntry> m_ghb;

      UInt32 m_tableSize;
      UInt32 m_tableHead; //next table position to be overwritten (in lack of a better replacement policy at the moment)
      std::vector<TableEntry> m_ghbTable;
};

#endif // __GHB_PREFETCHER_H
