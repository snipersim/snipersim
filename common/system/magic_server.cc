#include "magic_server.h"
#include "sim_api.h"
#include "simulator.h"
#include "thread_manager.h"
#include "logmem.h"
#include "performance_model.h"
#include "fastforward_performance_model.h"
#include "core_manager.h"
#include "dvfs_manager.h"
#include "hooks_manager.h"
#include "stats.h"
#include "timer.h"

MagicServer::MagicServer()
      : m_performance_enabled(false)
{
}

MagicServer::~MagicServer()
{ }

UInt64 MagicServer::Magic(thread_id_t thread_id, core_id_t core_id, UInt64 cmd, UInt64 arg0, UInt64 arg1)
{
   ScopedLock sl(Sim()->getThreadManager()->getLock());

   return Magic_unlocked(thread_id, core_id, cmd, arg0, arg1);
}

UInt64 MagicServer::Magic_unlocked(thread_id_t thread_id, core_id_t core_id, UInt64 cmd, UInt64 arg0, UInt64 arg1)
{
   switch(cmd)
   {
      case SIM_CMD_ROI_TOGGLE:
         if (Sim()->getConfig()->getSimulationROI() == Config::ROI_MAGIC)
         {
            return setPerformance(! m_performance_enabled);
         }
         else
         {
            return 0;
         }
      case SIM_CMD_ROI_START:
         Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_ROI_BEGIN, 0);
         if (Sim()->getConfig()->getSimulationROI() == Config::ROI_MAGIC)
         {
            return setPerformance(true);
         }
         else
         {
            return 0;
         }
      case SIM_CMD_ROI_END:
         Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_ROI_END, 0);
         if (Sim()->getConfig()->getSimulationROI() == Config::ROI_MAGIC)
         {
            return setPerformance(false);
         }
         else
         {
            return 0;
         }
      case SIM_CMD_MHZ_SET:
         return setFrequency(arg0 == UINT64_MAX ? core_id : arg0, arg1);
      case SIM_CMD_NAMED_MARKER:
      {
         char str[256];
         Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
         core->accessMemory(Core::NONE, Core::READ, arg1, str, 256, Core::MEM_MODELED_NONE);
         str[255] = '\0';

         MagicMarkerType args = { thread_id: thread_id, core_id: core_id, arg0: arg0, arg1: 0, str: str };
         Sim()->getHooksManager()->callHooks(HookType::HOOK_MAGIC_MARKER, (UInt64)&args);
         return 0;
      }
      case SIM_CMD_MARKER:
      {
         MagicMarkerType args = { thread_id: thread_id, core_id: core_id, arg0: arg0, arg1: arg1, str: NULL };
         Sim()->getHooksManager()->callHooks(HookType::HOOK_MAGIC_MARKER, (UInt64)&args);
         return 0;
      }
      case SIM_CMD_USER:
      {
         MagicMarkerType args = { thread_id: thread_id, core_id: core_id, arg0: arg0, arg1: arg1, str: NULL };
         return Sim()->getHooksManager()->callHooks(HookType::HOOK_MAGIC_USER, (UInt64)&args, true /* expect return value */);
      }
      case SIM_CMD_INSTRUMENT_MODE:
         return setInstrumentationMode(arg0);
      case SIM_CMD_MHZ_GET:
         return getFrequency(arg0 == UINT64_MAX ? core_id : arg0);
      default:
         LOG_ASSERT_ERROR(false, "Got invalid Magic %lu, arg0(%lu) arg1(%lu)", cmd, arg0, arg1);
   }
   return 0;
}

UInt64 MagicServer::getGlobalInstructionCount(void)
{
   UInt64 ninstrs = 0;
   for (UInt32 i = 0; i < Sim()->getConfig()->getApplicationCores(); i++)
      ninstrs += Sim()->getCoreManager()->getCoreFromID(i)->getInstructionCount();
   return ninstrs;
}

static Timer t_start;
UInt64 ninstrs_start;
__attribute__((weak)) void PinDetach(void) {}

void MagicServer::enablePerformance()
{
   Sim()->getStatsManager()->recordStats("roi-begin");
   ninstrs_start = getGlobalInstructionCount();
   t_start.start();

   Simulator::enablePerformanceModels();
   Sim()->setInstrumentationMode(InstMode::inst_mode_roi, true /* update_barrier */);
}

void MagicServer::disablePerformance()
{
   Simulator::disablePerformanceModels();
   Sim()->getStatsManager()->recordStats("roi-end");

   float seconds = t_start.getTime() / 1e9;
   UInt64 ninstrs = getGlobalInstructionCount() - ninstrs_start;
   printf("[SNIPER] Simulated %.1fM instructions @ %.1f KIPS (%.1f KIPS / target core - %.1fns/instr)\n",
      ninstrs / 1e6, ninstrs / seconds / 1e3,
      ninstrs / seconds / 1e3 / Sim()->getConfig()->getApplicationCores(),
      seconds * 1e9 / (float(ninstrs ? ninstrs : 1.) / Sim()->getConfig()->getApplicationCores()));

   PerformanceModel *perf = Sim()->getCoreManager()->getCoreFromID(0)->getPerformanceModel();
   if (perf->getFastforwardPerformanceModel()->getFastforwardedTime() > SubsecondTime::Zero())
   {
      // NOTE: Prints out the non-idle ratio for core 0 only, but it's just indicative anyway
      double ff_ratio = double(perf->getFastforwardPerformanceModel()->getFastforwardedTime().getNS())
                      / double(perf->getNonIdleElapsedTime().getNS());
      double percent_detailed = 100. * (1. - ff_ratio);
      printf("[SNIPER] Sampling: executed %.2f%% of simulated time in detailed mode\n", percent_detailed);
   }

   fflush(NULL);

   Sim()->setInstrumentationMode(InstMode::inst_mode_end, true /* update_barrier */);
   PinDetach();
}

void print_allocations();

UInt64 MagicServer::setPerformance(bool enabled)
{
   if (m_performance_enabled == enabled)
      return 1;

   m_performance_enabled = enabled;

   //static bool enabled = false;
   static Timer t_start;
   //ScopedLock sl(l_alloc);

   if (m_performance_enabled)
   {
      printf("[SNIPER] Enabling performance models\n");
      fflush(NULL);
      t_start.start();
      logmem_enable(true);
      Sim()->getHooksManager()->callHooks(HookType::HOOK_ROI_BEGIN, 0);
   }
   else
   {
      Sim()->getHooksManager()->callHooks(HookType::HOOK_ROI_END, 0);
      printf("[SNIPER] Disabling performance models\n");
      float seconds = t_start.getTime() / 1e9;
      printf("[SNIPER] Leaving ROI after %.2f seconds\n", seconds);
      fflush(NULL);
      logmem_enable(false);
      logmem_write_allocations();
   }

   if (enabled)
      enablePerformance();
   else
      disablePerformance();

   return 0;
}

UInt64 MagicServer::setFrequency(UInt64 core_number, UInt64 freq_in_mhz)
{
   UInt32 num_cores = Sim()->getConfig()->getApplicationCores();
   UInt64 freq_in_hz;
   if (core_number >= num_cores)
      return 1;
   freq_in_hz = 1000000 * freq_in_mhz;

   printf("[SNIPER] Setting frequency for core %" PRId64 " in DVFS domain %d to %" PRId64 " MHz\n", core_number, Sim()->getDvfsManager()->getCoreDomainId(core_number), freq_in_mhz);

   if (freq_in_hz > 0)
      Sim()->getDvfsManager()->setCoreDomain(core_number, ComponentPeriod::fromFreqHz(freq_in_hz));
   else {
      Sim()->getThreadManager()->stallThread_async(core_number, ThreadManager::STALL_BROKEN, SubsecondTime::MaxTime());
      Sim()->getCoreManager()->getCoreFromID(core_number)->setState(Core::BROKEN);
   }

   // First set frequency, then call hooks so hook script can find the new frequency by querying the DVFS manager
   Sim()->getHooksManager()->callHooks(HookType::HOOK_CPUFREQ_CHANGE, core_number);

   return 0;
}

UInt64 MagicServer::getFrequency(UInt64 core_number)
{
   UInt32 num_cores = Sim()->getConfig()->getApplicationCores();
   if (core_number >= num_cores)
      return UINT64_MAX;

   const ComponentPeriod *per = Sim()->getDvfsManager()->getCoreDomain(core_number);
   return per->getPeriodInFreqMHz();
}

UInt64 MagicServer::setInstrumentationMode(UInt64 sim_api_opt)
{
   InstMode::inst_mode_t inst_mode;
   switch (sim_api_opt)
   {
   case SIM_OPT_INSTRUMENT_DETAILED:
      inst_mode = InstMode::DETAILED;
      break;
   case SIM_OPT_INSTRUMENT_WARMUP:
      inst_mode = InstMode::CACHE_ONLY;
      break;
   case SIM_OPT_INSTRUMENT_FASTFORWARD:
      inst_mode = InstMode::FAST_FORWARD;
      break;
   default:
      LOG_PRINT_ERROR("Unexpected magic instrument opt type: %lx.", sim_api_opt);
   }
   Sim()->setInstrumentationMode(inst_mode, true /* update_barrier */);

   return 0;
}
