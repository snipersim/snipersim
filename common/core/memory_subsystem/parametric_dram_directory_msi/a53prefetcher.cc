#include "a53prefetcher.h"
#include "simulator.h"
#include "config.hpp"

inline intptr_t myAbs(intptr_t a) {
   return a < 0 ? -a:a;
}

A53Prefetcher::A53Prefetcher(String configName, core_id_t core_id)
   : m_cacheLineSize(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/cache_block_size", core_id))
   , m_patternLength(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/a53prefetcher/pattern_length", core_id)-1)
   , m_consecutivePatternLength(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/a53prefetcher/consecutive_pattern_length", core_id)-1)
   , m_numPrefetches(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/a53prefetcher/num_prefetches", core_id))
   , firstAddress(true)
   , stride(0)
   , currentPatternLength(0)
   , currentConsecutivePatternLength(0)
{
}

std::vector<IntPtr> A53Prefetcher::getNextAddress(IntPtr currentAddress, core_id_t core_id) {
   std::vector<IntPtr> prefetchAddress;

   if (firstAddress) {
      firstAddress = false;
   }
   else {
      intptr_t dist = currentAddress-prevAddress;
      if (dist == 0) {
      }
      else if (myAbs(dist) == m_cacheLineSize) {
         if (dist == stride) {
            ++currentConsecutivePatternLength;
         }
         else {
            stride = dist;
            currentConsecutivePatternLength = 1;
         }

         if (currentConsecutivePatternLength >= m_consecutivePatternLength) {
            for (unsigned int i = 1; i <= m_numPrefetches; ++i) {
               prefetchAddress.push_back(currentAddress + m_cacheLineSize*i);
            }
         }
      }
      else {
         if (dist == stride) {
            ++currentPatternLength;
         }
         else {
            stride = dist;
            currentPatternLength = 1;
         }

         if (currentPatternLength >= m_patternLength) {
            for (unsigned int i = 1; i <= m_numPrefetches; ++i) {
               prefetchAddress.push_back(currentAddress + stride*i);
            }
         }
      }
   }

   prevAddress = currentAddress;
   return prefetchAddress;
}
