#include "cheetah_model.h"

CheetahModel::CheetahModel(bool locked, unsigned min_size_bits, unsigned max_size_bits)
   : m_min_sets_log2(min_size_bits - associativity_log2 - line_size_log2)
   , m_max_sets_log2(max_size_bits - associativity_log2 - line_size_log2)
   , cheetah(associativity_log2, m_max_sets_log2, m_min_sets_log2, line_size_log2)
   , m_locked(locked)
{
}

CheetahModel::~CheetahModel()
{
}

void CheetahModel::updateStats(std::vector<UInt64> &stats)
{
   for(unsigned sets_log2 = m_min_sets_log2; sets_log2 <= m_max_sets_log2; ++sets_log2)
   {
      uint64_t size_bits = associativity_log2 + sets_log2 + line_size_log2;
      stats[size_bits] += cheetah.hits(sets_log2, 1 << associativity_log2);
   }
   stats[0] += cheetah.numentries();
}

void CheetahModel::accesses(IntPtr *addrs, int count)
{
   if (m_locked)
      m_lock.acquire();

   for(int i = 0; i < count; ++i)
      access(addrs[i]);

   if (m_locked)
      m_lock.release();
}

void CheetahModel::access(IntPtr address)
{
   cheetah.sacnmul_woarr(address);
}
