#include "bbv_count.h"

#include <algorithm>

// RNG parameters, defaults taken from drand48
#define RNG_A __UINT64_C(0x5DEECE66D)
#define RNG_C __UINT64_C(0xB)
#define RNG_M ((__UINT64_C(1) << 48) - 1)

// Same as drand48, but inlined for efficiency
inline uint64_t rng_next(uint64_t &state)
{
   state = (RNG_A * state + RNG_C) & RNG_M;
   return state >> 16;
}
inline uint64_t rng_seed(uint64_t seed)
{
   return (seed << 16) + 0x330E;
}


Bbv::Bbv()
   : m_instrs(0)
   , m_bbv_counts(NUM_BBV, 0)
{
}

void
Bbv::count(uint64_t address, uint64_t count)
{
   m_instrs += count;

   // Perform random projection of basic-block vectors onto NUM_BBV dimensions
   // As there are too many BBVs, we cannot store the projection matrix, rather,
   // we re-generate it on request using an RNG seeded with the BBV address.
   // Since this is a hot loop in FAST_FORWARD mode, use an inlined RNG
   // and four parallel code paths to exploit as much ILP as possible.
   uint64_t s0 = rng_seed(address), s1 = rng_seed(address + 1), s2 = rng_seed(address + 2), s3 = rng_seed(address + 3);
   for(int i = 0; i < NUM_BBV; i += 4)
   {
      uint64_t weight = rng_next(s0);
      m_bbv_counts[i] += (weight & 0xffff) * count;
      weight = rng_next(s1);
      m_bbv_counts[i+1] += (weight & 0xffff) * count;
      weight = rng_next(s2);
      m_bbv_counts[i+2] += (weight & 0xffff) * count;
      weight = rng_next(s3);
      m_bbv_counts[i+3] += (weight & 0xffff) * count;
   }
}

void
Bbv::clear()
{
   m_instrs = 0;
   for(int i = 0; i < Bbv::NUM_BBV; ++i)
      m_bbv_counts[i] = 0;
}
