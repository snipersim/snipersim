#ifndef A53PREFETCHER_H
#define A53PREFETCHER_H

#include "prefetcher.h"

class A53Prefetcher : public Prefetcher
{
   const unsigned int m_cacheLineSize;
   const unsigned int m_patternLength, m_consecutivePatternLength;
   const unsigned int m_numPrefetches;
   bool firstAddress;
   intptr_t stride;
   IntPtr prevAddress;
   unsigned int currentPatternLength, currentConsecutivePatternLength;

public:
   A53Prefetcher(String configName, core_id_t core_id);
   std::vector<IntPtr> getNextAddress(IntPtr currentAddress, core_id_t core_id) override;
};

#endif // A53PREFETCHER_H
