#ifndef TLB_H
#define TLB_H

#include "fixed_types.h"
#include "cache.h"

namespace ParametricDramDirectoryMSI
{
   class TLB
   {
      private:
         static const UInt32 SIM_PAGE_SHIFT = 12; // 4KB
         static const IntPtr SIM_PAGE_SIZE = (1L << SIM_PAGE_SHIFT);
         static const IntPtr SIM_PAGE_MASK = ~(SIM_PAGE_SIZE - 1);

         UInt32 m_size;
         UInt32 m_associativity;
         Cache m_cache;

         TLB *m_next_level;

         UInt64 m_access, m_miss;
      public:
         TLB(String name, String cfgname, core_id_t core_id, UInt32 num_entries, UInt32 associativity, TLB *next_level);
         bool lookup(IntPtr address, SubsecondTime now, bool allocate_on_miss = true);
         void allocate(IntPtr address, SubsecondTime now);
   };
}

#endif // TLB_H
