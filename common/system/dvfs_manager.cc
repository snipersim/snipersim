
#include <cassert>

#include "dvfs_manager.h"
#include "simulator.h"
#include "core_manager.h"
#include "core.h"
#include "performance_model.h"
#include "instruction.h"
#include "log.h"
#include "config.hpp"

// This manager seperates DVFS domains by socket, each getting it's own domain
// Therefore, one core per socket translates to one domain per core
#define DEFAULT_NUM_CORES_PER_SOCKET 1

DvfsManager::DvfsManager()
{
   m_num_local_cores = Config::getSingleton()->getNumLocalCores();
   m_num_app_cores = Config::getSingleton()->getNumLocalApplicationCores();

   m_cores_per_socket = Sim()->getCfg()->getInt("dvfs/simple/cores_per_socket", DEFAULT_NUM_CORES_PER_SOCKET);
   m_transition_latency = SubsecondTime::NS() * Sim()->getCfg()->getInt("dvfs/transition_latency", 0);

   LOG_ASSERT_ERROR("simple" == Sim()->getCfg()->getString("dvfs/type"), "Currently, only this simple dvfs scheme is defined");

   // Initial configuration provides for socket-wide frequency control, with [dvfs/simple/cores_per_socket] cores per socket
   m_num_proc_domains = m_num_app_cores / m_cores_per_socket;
   if (m_num_app_cores % m_cores_per_socket != 0)
   {
      // Round up if necessary
      m_num_proc_domains++;
   }

   float core_frequency = Sim()->getCfg()->getFloat("perf_model/core/frequency");
   // Create a domain, converting from GHz frequencies specified in the configuration to Hz
   ComponentPeriod core_period = ComponentPeriod::fromFreqHz(core_frequency*1000000000);

   // Allocate m_num_proc_domains clock domains, each with core_period as the initial frequency/period
   app_proc_domains.resize(m_num_proc_domains, core_period);

   // Allow per-core initial frequency overrides
   for(unsigned int i = 0; i < m_num_app_cores; ++i) {
      float _core_frequency = Sim()->getCfg()->getFloat("perf_model/core" + itostr(i) + "/frequency", -1);
      if (_core_frequency > 0) {
         app_proc_domains[i] = ComponentPeriod::fromFreqHz(_core_frequency*1000000000);
         printf("Core %d at %.2f GHz (global clock %.2f GHz)\n", i, _core_frequency, core_frequency);
      }
   }

   // Allocate global domains for all other non-application processors
   global_domains.resize(DOMAIN_GLOBAL_MAX, core_period);
}

UInt32 DvfsManager::getCoreDomainId(UInt32 core_id)
{
   LOG_ASSERT_ERROR(core_id < m_num_app_cores, "Core domain ids are only supported for application process domains")

   return core_id / m_cores_per_socket;
}

// core_id, 0-indexed
const ComponentPeriod* DvfsManager::getCoreDomain(UInt32 core_id)
{
   LOG_ASSERT_ERROR(Sim()->getConfig()->getCurrentProcessNum() == 0
         || Sim()->getConfig()->getCurrentProcessNum() == Sim()->getConfig()->getProcessNumForCore(core_id),
      "Core domain requested for core %d which is in another process", core_id);

   if (core_id < m_num_app_cores)
   {
      return &app_proc_domains[getCoreDomainId(core_id)];
   }
   else
   {
      // We currently only support a single non-app domain
      return &global_domains[DOMAIN_GLOBAL_DEFAULT];
   }
}

const ComponentPeriod* DvfsManager::getGlobalDomain(DvfsGlobalDomain domain_id)
{
   LOG_ASSERT_ERROR(UInt32(domain_id) < global_domains.size(),
      "Global domain %d requested, only %d exist", domain_id, global_domains.size());

   return &global_domains[domain_id];
}

void DvfsManager::setCoreDomain(UInt32 core_id, ComponentPeriod new_freq)
{
   LOG_ASSERT_ERROR(Sim()->getConfig()->getCurrentProcessNum() == 0
         || Sim()->getConfig()->getCurrentProcessNum() == Sim()->getConfig()->getProcessNumForCore(core_id),
      "Core domain requested for core %d which is in another process", core_id);

   if (core_id < m_num_app_cores)
   {
      app_proc_domains[getCoreDomainId(core_id)] = new_freq;

      if (Sim()->getConfig()->getCurrentProcessNum() == Sim()->getConfig()->getProcessNumForCore(core_id)) {
         /* queue a fake instruction that will account for the transition latency */
         Instruction *i = new SyncInstruction(m_transition_latency, SyncInstruction::DVFS_TRANSITION);
         Sim()->getCoreManager()->getCoreFromID(core_id)->getPerformanceModel()->queueDynamicInstruction(i);
      }
   }
   else
   {
      // We currently only support a single non-app domain
      LOG_PRINT_ERROR("Cannot change non-core frequency");
   }
}
