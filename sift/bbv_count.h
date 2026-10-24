#ifndef BBV_COUNT_H
#define BBV_COUNT_H

#define __STDC_LIMIT_MACROS
#include <stdint.h>
#include <vector>

class Bbv
{
   private:
      uint64_t m_instrs;
      std::vector<uint64_t> m_bbv_counts;

   public:
      // Number of dimensions to project BBVs to
      // Make sure this remains a multiple of four, or update the unrolled loop in BbvCount::count
      static const int NUM_BBV = 16;

      Bbv();
      void count(uint64_t address, uint64_t count);
      void clear();

      uint64_t getDimension(int dim) const { return m_bbv_counts.at(dim); }
      uint64_t getInstructionCount(void) const { return m_instrs; }
};

#endif // BBV_COUNT_H
