#include "ghb_prefetcher.h"
#include "simulator.h"
#include "config.hpp"

GhbPrefetcher::GhbPrefetcher(String configName, core_id_t core_id)
   : m_prefetchWidth(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/ghb/width", core_id))
   , m_prefetchDepth(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/ghb/depth", core_id))
   , m_lastAddress(INVALID_ADDRESS)
   , m_ghbSize(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/ghb/ghb_size", core_id))
   , m_ghbHead(0)
   , m_ghb(m_ghbSize)
   , m_tableSize(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/ghb/ghb_table_size", core_id))
   , m_tableHead(0)
   , m_ghbTable(m_tableSize)
{
}

GhbPrefetcher::~GhbPrefetcher()
{
}

std::vector<IntPtr>
GhbPrefetcher::getNextAddress(IntPtr currentAddress)
{
   std::vector<IntPtr> prefetchList;

   //deal with prefether initialization
   if (m_lastAddress == INVALID_ADDRESS)
   {
      m_lastAddress = currentAddress;
      return prefetchList;
   }

   //determine the delta with the last address
   SInt64 delta = currentAddress - m_lastAddress;
   m_lastAddress = currentAddress;

   //look for the current delta in the table
   UInt32 i = 0;
   while (i < m_tableSize &&
          m_ghbTable[i].delta != INVALID_DELTA &&
          m_ghbTable[i].delta != delta)
      ++i;

   if (i != m_tableSize && m_ghbTable[i].delta == delta)
   {
      UInt32 width = 0;
      UInt32 ghbIndex = m_ghbTable[i].ghbIndex;

      while(width < m_prefetchWidth && ghbIndex != INVALID_INDEX)
      {
         UInt32 depth = 0;

         IntPtr newAddress = currentAddress;

         while (depth < m_prefetchDepth && m_ghb[(ghbIndex + depth)%m_ghbSize].delta != INVALID_DELTA)
         {
            newAddress += m_ghb[(ghbIndex + depth)%m_ghbSize].delta;

            //add address to the list if it wasn't in there already
            if (find(prefetchList.begin(), prefetchList.end(), newAddress) == prefetchList.end())
               prefetchList.push_back(newAddress);

            ++depth;
         }

         ++width;
         ghbIndex = m_ghb[ghbIndex].nextIndex;
      }
   }

   //add new delta to the ghb and table

   m_ghb[m_ghbHead].delta = delta;
   m_ghb[m_ghbHead].nextIndex = INVALID_INDEX;

   UInt32 prevHead = m_ghbHead > 0 ? m_ghbHead - 1 : m_ghbSize - 1;
   SInt64 prevDelta = m_ghb[prevHead].delta;

   if (prevDelta != INVALID_DELTA)
   {
      i = 0;
      while (i < m_tableSize &&
             m_ghbTable[i].delta != INVALID_DELTA &&
             m_ghbTable[i].delta != prevDelta)
         ++i;

      if (i != m_tableSize && m_ghbTable[i].delta == prevDelta)
      { //update existing entry
         m_ghb[m_ghbHead].nextIndex = m_ghbTable[i].ghbIndex;
         m_ghbTable[i].ghbIndex = m_ghbHead;
      }
      else
      { //prevDelta not found ==> add entry to table
         m_ghbTable[m_tableHead].delta = prevDelta;
         m_ghbTable[m_tableHead].ghbIndex = m_ghbHead;

         m_tableHead = (m_tableHead + 1) % m_tableSize;
      }

   }

   m_ghbHead = (m_ghbHead + 1) % m_ghbSize;

   return prefetchList;
}
