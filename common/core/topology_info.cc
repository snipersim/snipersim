#include "topology_info.h"
#include "simulator.h"
#include "config.h"
#include "config.hpp"
#include "core_manager.h"

#define VERBOSE 0

SInt32 TopologyInfo::s_package = -1, TopologyInfo::s_cores_this_package = 0, TopologyInfo::s_core_id_last = -1;

void TopologyInfo::setup(UInt32 smt_cores, UInt32 llc_sharers)
{
   UInt32 id = core_id;

   // SMT level
   this->smt_index = id % smt_cores;
   this->smt_count = smt_cores;
   id /= smt_cores;

   // Core level
   UInt32 cores_per_package;
   if (Sim()->getCfg()->getString("network/memory_model_1") == "emesh_hop_by_hop")
      // Mesh NoC: assume single chip
      cores_per_package = Sim()->getConfig()->getApplicationCores();
   else
      // Other network: assume one LLC per package
      cores_per_package = llc_sharers;
   cores_per_package /= smt_cores;
   this->core_index = id % cores_per_package;
   this->core_count = cores_per_package;

   // Package level
   // Due to heterogeneity, we cannot directly compute our package #
   // Assume cores are initialized in-order, and figure out where the previous cores are
   LOG_ASSERT_ERROR(s_core_id_last == core_id - 1, "Cores not initialized in order");
   if (s_cores_this_package == 0)
   {
      // First core of a new package
      ++s_package;
      s_cores_this_package = cores_per_package * smt_count;
   }
   --s_cores_this_package;
   s_core_id_last = core_id;
   this->package = s_package;

   LOG_ASSERT_ERROR(smt_cores <= (1 << SMT_SHIFT_BITS), "Too many smt_cores, increase SMT_SHIFT_BITS");
   LOG_ASSERT_ERROR((cores_per_package << SMT_SHIFT_BITS) <= (1 << PACKAGE_SHIFT_BITS), "Too many cores_per_package, increase PACKAGE_SHIFT_BITS");
   this->apic_id = (this->package << PACKAGE_SHIFT_BITS) | (this->core_index << SMT_SHIFT_BITS) | this->smt_index;

   #if VERBOSE
   printf("CORE %d: SMT %d/%d CORE %d/%d PACKAGE %d APICID %d\n", core_id, smt_index, smt_count, core_index, core_count, package, apic_id);
   #endif
}
