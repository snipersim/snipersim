#ifndef PREFETCHER_H
#define PREFETCHER_H

#include "fixed_types.h"

class Prefetcher
{
   public:
      Prefetcher();
      IntPtr getNextAddress(IntPtr current_address);
      bool hasAddress(IntPtr address);

   private:
      static const UInt32 n_flows = 4;
      UInt32 n_flow_next;
      IntPtr m_prev_address[n_flows];
};

#endif // PREFETCHER_H
