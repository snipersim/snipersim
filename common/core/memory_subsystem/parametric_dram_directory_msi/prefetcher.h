#ifndef PREFETCHER_H
#define PREFETCHER_H

#include "fixed_types.h"

#include <vector>

class Prefetcher
{
   public:
      Prefetcher(String configName, core_id_t core_id);
      IntPtr getNextAddress(IntPtr current_address);

   private:
      const UInt32 n_flows;
      const bool stop_at_page;
      UInt32 n_flow_next;
      std::vector<IntPtr> m_prev_address;
};

#endif // PREFETCHER_H
