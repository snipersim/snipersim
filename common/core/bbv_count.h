#ifndef BBV_COUNT_H
#define BBV_COUNT_H

#include "fixed_types.h"

#include <vector>

class BbvCount
{
   private:
      const core_id_t m_core_id;

      // Running count (never reset)
      UInt64 m_instrs_abs;
      std::vector<UInt64> m_bbv_counts_abs;

      // Value of icount/BBV at last reset
      UInt64 m_instrs_reset;
      std::vector<UInt64> m_bbv_reset;

      // Relative BBV ending at last reset
      std::vector<UInt64> m_bbv_previous;

      UInt64 m_sample_period;
      UInt64 m_sample_seed;
      UInt64 m_sample;

      void sampleReset();

   public:
      // Number of dimensions to project BBVs to
      // Make sure this remains a multiple of four, or update the unrolled loop in BbvCount::count
      static const int NUM_BBV = 16;

      BbvCount(core_id_t core_id);
      ~BbvCount();

      bool sample();
      void count(UInt64 address, UInt64 count);
      void reset(bool save = true);
      UInt64 getDiff();
      UInt64 getDimension(int dim) { return m_bbv_counts_abs.at(dim) - m_bbv_reset.at(dim); }
      UInt64 getInstructionCount(void) const { return m_instrs_abs - m_instrs_reset; }
};

#endif // BBV_COUNT_H
