#include "simple_prefetcher.h"
#include "simulator.h"
#include "config.hpp"

#include <cstdlib>

const IntPtr PAGE_SIZE = 4096;
const IntPtr PAGE_MASK = ~(PAGE_SIZE-1);

SimplePrefetcher::SimplePrefetcher(String configName, core_id_t core_id)
   : n_flows(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/simple/flows", core_id))
   , num_prefetches(Sim()->getCfg()->getIntArray("perf_model/" + configName + "/prefetcher/simple/num_prefetches", core_id))
   , stop_at_page(Sim()->getCfg()->getBoolArray("perf_model/" + configName + "/prefetcher/simple/stop_at_page_boundary", core_id))
   , n_flow_next(0)
   , m_prev_address(n_flows)
{
}

std::vector<IntPtr>
SimplePrefetcher::getNextAddress(IntPtr current_address)
{
   UInt32 n_flow = n_flow_next;
   IntPtr min_dist = PAGE_SIZE;
   n_flow_next = (n_flow_next + 1) % n_flows; // Round robin replacement

   // Find the nearest address in our list of previous addresses
   for(UInt32 i = 0; i < n_flows; ++i)
   {
      IntPtr dist = abs(current_address - m_prev_address[i]);
      if (dist < min_dist)
      {
         n_flow = i;
         min_dist = dist;
      }
   }

   // Now, n_flow points to the previous address of the best matching flow
   // (Or, if none matched, the round-robin determined one to replace)

   // Simple linear stride prefetcher
   IntPtr stride = current_address - m_prev_address[n_flow];
   m_prev_address[n_flow] = current_address;

   std::vector<IntPtr> addresses;
   if (stride != 0)
   {
      for(unsigned int i = 0; i < num_prefetches; ++i)
      {
         IntPtr prefetch_address = current_address + i * stride;
         // But stay within the page if requested
         if (!stop_at_page || ((prefetch_address & PAGE_MASK) == (current_address & PAGE_MASK)))
            addresses.push_back(prefetch_address);
      }
   }

   return addresses;
}
