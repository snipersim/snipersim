#include "simulator.h"
#include "log.h"
#include "core.h"
#include "core_manager.h"
#include "thread_manager.h"
#include "sync_server.h"
#include "syscall_server.h"
#include "magic_server.h"
#include "sim_thread_manager.h"
#include "clock_skew_minimization_object.h"
#include "fastforward_performance_manager.h"
#include "fxsupport.h"
#include "timer.h"
#include "stats.h"
#include "thread_stats_manager.h"
#include "pthread_emu.h"
#include "trace_manager.h"
#include "dvfs_manager.h"
#include "hooks_manager.h"
#include "sampling_manager.h"
#include "fault_injection.h"
#include "routine_tracer.h"
#include "instruction.h"
#include "config.hpp"
#include "magic_client.h"
#include "tags.h"
#include "instruction_tracer.h"
#include "memory_tracker.h"
#include "circular_log.h"

#include <sstream>

Simulator *Simulator::m_singleton;
config::Config *Simulator::m_config_file;
bool Simulator::m_config_file_allowed = true;
Config::SimulationMode Simulator::m_mode;

void Simulator::allocate()
{
   assert(m_singleton == NULL);
   m_singleton = new Simulator();
}

void Simulator::setConfig(config::Config *cfg, Config::SimulationMode mode)
{
   m_config_file = cfg;
   m_mode = mode;
}

void Simulator::release()
{
   m_singleton->m_running = false;
   // Fxsupport::fini();
   delete m_singleton;
   m_singleton = NULL;
}

Simulator::Simulator()
   : m_config(m_mode)
   , m_log(m_config)
   , m_tags_manager(new TagsManager(m_config_file))
   , m_stats_manager(new StatsManager)
   , m_transport(NULL)
   , m_core_manager(NULL)
   , m_thread_manager(NULL)
   , m_thread_stats_manager(NULL)
   , m_sim_thread_manager(NULL)
   , m_clock_skew_minimization_manager(NULL)
   , m_fastforward_performance_manager(NULL)
   , m_trace_manager(NULL)
   , m_dvfs_manager(NULL)
   , m_hooks_manager(NULL)
   , m_sampling_manager(NULL)
   , m_faultinjection_manager(NULL)
   , m_rtn_tracer(NULL)
   , m_memory_tracker(NULL)
   , m_running(false)
   , m_inst_mode_output(true)
{
}

void Simulator::start()
{
   LOG_PRINT("In Simulator ctor.");

   m_hooks_manager = new HooksManager();
   m_syscall_server = new SyscallServer();
   m_sync_server = new SyncServer();
   m_magic_server = new MagicServer();
   m_transport = Transport::create();
   m_dvfs_manager = new DvfsManager();
   m_faultinjection_manager = FaultinjectionManager::create();
   m_thread_manager = new ThreadManager();
   m_thread_stats_manager = new ThreadStatsManager();
   m_clock_skew_minimization_manager = ClockSkewMinimizationManager::create();
   m_clock_skew_minimization_server = ClockSkewMinimizationServer::create();
   m_core_manager = new CoreManager();
   m_sim_thread_manager = new SimThreadManager();
   m_sampling_manager = new SamplingManager();
   m_fastforward_performance_manager = FastForwardPerformanceManager::create();
   m_rtn_tracer = RoutineTracer::create();

   if (Sim()->getCfg()->getBool("traceinput/enabled"))
      m_trace_manager = new TraceManager();
   else
      m_trace_manager = NULL;

   CircularLog::enableCallbacks();

   InstructionTracer::init();

   Fxsupport::init();

   PthreadEmu::init();

   m_hooks_manager->init();
   if (m_trace_manager)
      m_trace_manager->init();

   m_sim_thread_manager->spawnSimThreads();

   Instruction::initializeStaticInstructionModel();

   InstMode::inst_mode_init = InstMode::fromString(getCfg()->getString("general/inst_mode_init"));
   InstMode::inst_mode_roi  = InstMode::fromString(getCfg()->getString("general/inst_mode_roi"));
   InstMode::inst_mode_end  = InstMode::fromString(getCfg()->getString("general/inst_mode_end"));
   m_inst_mode_output = getCfg()->getBool("general/inst_mode_output");

   printInstModeSummary();
   setInstrumentationMode(InstMode::inst_mode_init, true /* update_barrier */);

   /* Save a copy of the configuration for reference */
   m_config_file->saveAs(m_config.formatOutputFileName("sim.cfg"));

// PIN_SpawnInternalThread doesn't schedule its threads until after PIN_StartProgram
//   m_transport->barrier();

   m_hooks_manager->callHooks(HookType::HOOK_SIM_START, 0);
   m_stats_manager->recordStats("start");
   if (Sim()->getFastForwardPerformanceManager())
   {
      Sim()->getFastForwardPerformanceManager()->enable();
   }
   if (Sim()->getConfig()->getSimulationROI() == Config::ROI_FULL)
   {
      // roi-begin
      Sim()->getMagicServer()->setPerformance(true);
   }

   m_running = true;
}

Simulator::~Simulator()
{
   // Done with all the Pin stuff, allow using Config::Config again
   m_config_file_allowed = true;

   // In case we're still in ROI (ROI is the full application, or someone forgot to turn it off), end ROI now
   if (getMagicServer()->inROI())
   {
      // roi-end
      getMagicServer()->setPerformance(false);
   }

   m_stats_manager->recordStats("stop");
   m_hooks_manager->callHooks(HookType::HOOK_SIM_END, 0);

   TotalTimer::reports();

   LOG_PRINT("Simulator dtor starting...");

   m_hooks_manager->fini();

   if (m_clock_skew_minimization_manager)
   {
      delete m_clock_skew_minimization_manager; m_clock_skew_minimization_manager = NULL;
   }
   if (m_clock_skew_minimization_server)
   {
      delete m_clock_skew_minimization_server;  m_clock_skew_minimization_server = NULL;
   }

   m_sim_thread_manager->quitSimThreads();

   m_transport->barrier();

   if (m_rtn_tracer)
   {
      delete m_rtn_tracer;             m_rtn_tracer = NULL;
   }
   delete m_trace_manager;             m_trace_manager = NULL;
   delete m_sampling_manager;          m_sampling_manager = NULL;
   if (m_faultinjection_manager)
   {
      delete m_faultinjection_manager; m_faultinjection_manager = NULL;
   }
   delete m_sim_thread_manager;        m_sim_thread_manager = NULL;
   delete m_thread_manager;            m_thread_manager = NULL;
   delete m_thread_stats_manager;      m_thread_stats_manager = NULL;
   delete m_core_manager;              m_core_manager = NULL;
   delete m_dvfs_manager;              m_dvfs_manager = NULL;
   delete m_magic_server;              m_magic_server = NULL;
   delete m_sync_server;               m_sync_server = NULL;
   delete m_syscall_server;            m_syscall_server = NULL;
   delete m_hooks_manager;             m_hooks_manager = NULL;
   delete m_tags_manager;              m_tags_manager = NULL;
   delete m_transport;                 m_transport = NULL;
   delete m_stats_manager;             m_stats_manager = NULL;
}

void Simulator::enablePerformanceModels()
{
   if (Sim()->getFastForwardPerformanceManager() && InstMode::inst_mode_roi == InstMode::DETAILED)
      Sim()->getFastForwardPerformanceManager()->disable();
   for (UInt32 i = 0; i < Sim()->getConfig()->getTotalCores(); i++)
      Sim()->getCoreManager()->getCoreFromID(i)->enablePerformanceModels();
}

void Simulator::disablePerformanceModels()
{
   for (UInt32 i = 0; i < Sim()->getConfig()->getTotalCores(); i++)
      Sim()->getCoreManager()->getCoreFromID(i)->disablePerformanceModels();
   if (Sim()->getFastForwardPerformanceManager() && InstMode::inst_mode_roi == InstMode::DETAILED)
      Sim()->getFastForwardPerformanceManager()->enable();
}

void Simulator::setInstrumentationMode(InstMode::inst_mode_t new_mode, bool update_barrier)
{
   if (new_mode != InstMode::inst_mode)
   {
      if (m_inst_mode_output && InstMode::inst_mode != InstMode::INVALID)
      {
         printf("[SNIPER] Setting instrumentation mode to %s\n", inst_mode_names[new_mode]); fflush(stdout);
      }
      InstMode::inst_mode = new_mode;

      if (Sim()->getConfig()->getSimulationMode() == Config::PINTOOL)
         InstMode::updateInstrumentationMode();

      // If there is a fast-forward performance model, it needs to take care of barrier synchronization.
      // If we're called with update_barrier == false, the caller (SamplingManager) manages the barrier.
      // Else, disable the barrier in fast-forward/cache-only
      if (update_barrier && !Sim()->getFastForwardPerformanceManager())
         getClockSkewMinimizationServer()->setDisable(new_mode != InstMode::DETAILED);

      Sim()->getHooksManager()->callHooks(HookType::HOOK_INSTRUMENT_MODE, (UInt64)new_mode);
   }
}

void Simulator::printInstModeSummary()
{
   printf("[SNIPER] --------------------------------------------------------------------------------\n");
   printf("[SNIPER] Sniper using ");
   switch(getConfig()->getSimulationMode())
   {
      case Config::PINTOOL:
         printf("Pin");
         break;
      case Config::STANDALONE:
         printf("SIFT/trace-driven");
         break;
      default:
         LOG_PRINT_ERROR("Unknown simulation mode");
   }
   printf(" frontend\n");
   switch(getConfig()->getSimulationROI())
   {
      case Config::ROI_FULL:
         printf("[SNIPER] Running full application in %s mode\n", inst_mode_names[InstMode::inst_mode_roi]);
         break;
      case Config::ROI_MAGIC:
         printf("[SNIPER] Running pre-ROI region in  %s mode\n", inst_mode_names[InstMode::inst_mode_init]);
         printf("[SNIPER] Running application ROI in %s mode\n", inst_mode_names[InstMode::inst_mode_roi]);
         printf("[SNIPER] Running post-ROI region in %s mode\n", inst_mode_names[InstMode::inst_mode_end]);
         break;
      case Config::ROI_SCRIPT:
         printf("[SNIPER] Running in script-driven instrumenation mode (--roi-script)\n");
         printf("[SNIPER] Using %s mode for warmup\n", inst_mode_names[InstMode::inst_mode_init]);
         printf("[SNIPER] Using %s mode for detailed\n", inst_mode_names[InstMode::inst_mode_roi]);
         // Script should print something here...
         break;
      default:
         LOG_PRINT_ERROR("Unknown ROI mode");
   }
   printf("[SNIPER] --------------------------------------------------------------------------------\n");
}
