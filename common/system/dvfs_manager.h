#ifndef __DVFS_MANAGER_H
#define __DVFS_MANAGER_H

#include "subsecond_time.h"

#include <vector>

// Each process has a copy of all global frequencies, and of the core frequencies local to that process
// In addition, process 0 has a copy of all core frequencies so as to quickly fulfill queries from the MCP/scripts

class DvfsManager
{
public:
   enum DvfsGlobalDomain {
      DOMAIN_GLOBAL_DEFAULT,
      // If we wanted separate domains for e.g. DRAM, add them here and initialize them in DvfsManager::DvfsManager()
      DOMAIN_GLOBAL_MAX
   };
   DvfsManager();
   UInt32 getCoreDomainId(UInt32 core_id);
   const ComponentPeriod* getCoreDomain(UInt32 core_id);
   const ComponentPeriod* getGlobalDomain(DvfsGlobalDomain domain_id = DOMAIN_GLOBAL_DEFAULT);
protected:
   // Make sure all frequency updates pass through the correct path
   void setCoreDomain(UInt32 core_id, ComponentPeriod new_freq);
   friend class MagicServer;
private:
   UInt32 m_cores_per_socket;
   SubsecondTime m_transition_latency;
   UInt32 m_num_proc_domains;
   UInt32 m_num_app_cores;
   std::vector<ComponentPeriod> app_proc_domains;
   std::vector<ComponentPeriod> global_domains;
};

#endif /* __DVFS_MANAGER_H */
