#ifndef __CHEETAH_MODEL_H
#define __CHEETAH_MODEL_H

#include "fixed_types.h"
#include "saclru.h"
#include "lock.h"

#include <vector>

class CheetahModel
{
   private:
      static const unsigned associativity_log2 = 4,
                            line_size_log2 = 6;
      const unsigned m_min_sets_log2,
                     m_max_sets_log2;
      CheetahSACLRU cheetah;
      bool m_locked;
      Lock m_lock;

      void access(IntPtr addr);

   public:
      static unsigned getMinSize() { return associativity_log2 + line_size_log2; }

      CheetahModel(bool locked, unsigned min_size_bits, unsigned max_size_bits);
      ~CheetahModel();

      void accesses(IntPtr *addrs, int count);
      void updateStats(std::vector<UInt64> &stats);
};

#endif // __CHEETAH_MODEL_H
