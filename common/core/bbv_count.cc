#include "bbv_count.h"
#include "simulator.h"
#include "config.hpp"
#include "stats.h"
#include "rng.h"

BbvCount::BbvCount(core_id_t core_id)
   : m_core_id(core_id)
   , m_instrs_abs(0)
   , m_bbv_counts_abs(NUM_BBV, 0)
   , m_instrs_reset(0)
   , m_bbv_reset(NUM_BBV, 0)
   , m_bbv_previous(NUM_BBV, 0)
   // Define N to skip X samples with X uniformely distributed between 0..2*N, so on average 1/N samples
   , m_sample_period(2 * Sim()->getCfg()->getInt("bbv/sampling"))
   , m_sample_seed(rng_seed(m_sample_period))
{
   sampleReset();
   for(int i = 0; i < NUM_BBV; ++i)
      registerStatsMetric("core", core_id, "bbv-"+itostr(i), &m_bbv_counts_abs[i]);
}

BbvCount::~BbvCount()
{
#if 0
   printf("BBV: ");
   for(int i = 0; i < NUM_BBV; ++i)
      printf(" %ld", m_bbv_counts[i]);
   printf("\n");
#endif
}

void
BbvCount::sampleReset()
{
   if (m_sample_period) {
      m_sample = rng_next(m_sample_seed) % m_sample_period;
   } else {
      m_sample = 0;
   }
}

bool
BbvCount::sample()
{
   return m_sample-- == 0;
}

void
BbvCount::count(UInt64 address, UInt64 count)
{
   sampleReset();

   m_instrs_abs += count;

   // Perform random projection of basic-block vectors onto NUM_BBV dimensions
   // As there are too many BBVs, we cannot store the projection matrix, rather,
   // we re-generate it on request using an RNG seeded with the BBV address.
   // Since this is a hot loop in FAST_FORWARD mode, use an inlined RNG
   // and four parallel code paths to exploit as much ILP as possible.
   UInt64 s0 = rng_seed(address), s1 = rng_seed(address + 1), s2 = rng_seed(address + 2), s3 = rng_seed(address + 3);
   for(int i = 0; i < NUM_BBV; i += 4)
   {
      UInt64 weight = rng_next(s0);
      m_bbv_counts_abs[i] += (weight & 0xffff) * count;
      weight = rng_next(s1);
      m_bbv_counts_abs[i+1] += (weight & 0xffff) * count;
      weight = rng_next(s2);
      m_bbv_counts_abs[i+2] += (weight & 0xffff) * count;
      weight = rng_next(s3);
      m_bbv_counts_abs[i+3] += (weight & 0xffff) * count;
   }
}

void
BbvCount::reset(bool save)
{
   for(int i = 0; i < NUM_BBV; ++i)
   {
      if (save)
      {
         UInt64 icount = getInstructionCount();
         m_bbv_previous[i] = getDimension(i) / (icount ? icount : 1);
      }
      m_bbv_reset[i] = m_bbv_counts_abs[i];
   }
   m_instrs_reset = m_instrs_abs;
}

UInt64
BbvCount::getDiff()
{
   UInt64 diff = 0;
   UInt64 icount = getInstructionCount();

   for(int i = 0; i < NUM_BBV; ++i)
   {
         UInt64 bbv_count = getDimension(i) / (icount ? icount : 1);
         diff += abs(bbv_count - m_bbv_previous[i]);
   }

   // Normalize by the number of BBVs to keep the results consistent as the count changes
   return diff / NUM_BBV;
}
