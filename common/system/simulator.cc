#include "simulator.h"
#include "log.h"
#include "lcp.h"
#include "mcp.h"
#include "core.h"
#include "core_manager.h"
#include "thread_manager.h"
#include "perf_counter_manager.h"
#include "sim_thread_manager.h"
#include "clock_skew_minimization_object.h"
#include "fxsupport.h"
#include "timer.h"
#include "stats.h"
#include "pthread_emu.h"
#include "trace_manager.h"
#include "dvfs_manager.h"
#include "hooks_manager.h"
#include "instruction.h"
#include "config.hpp"

#include <sstream>

Simulator *Simulator::m_singleton;
config::Config *Simulator::m_config_file;
bool Simulator::m_config_file_allowed = true;
Config::SimulationMode Simulator::m_mode = Config::SimulationMode::FROM_CONFIG;

static UInt64 getTime()
{
   timeval t;
   gettimeofday(&t, NULL);
   UInt64 time = (((UInt64)t.tv_sec) * 1000000 + t.tv_usec);
   return time;
}

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
   // Fxsupport::fini();
   delete m_singleton;
   m_singleton = NULL;
}

Simulator::Simulator()
   : m_mcp(NULL)
   , m_mcp_thread(NULL)
   , m_lcp(NULL)
   , m_lcp_thread(NULL)
   , m_config(m_mode)
   , m_log(m_config)
   , m_stats_manager(new StatsManager)
   , m_transport(NULL)
   , m_core_manager(NULL)
   , m_thread_manager(NULL)
   , m_perf_counter_manager(NULL)
   , m_sim_thread_manager(NULL)
   , m_clock_skew_minimization_manager(NULL)
   , m_trace_manager(NULL)
   , m_dvfs_manager(NULL)
   , m_hooks_manager(NULL)
   , m_finished(false)
   , m_boot_time(getTime())
   , m_start_time(0)
   , m_stop_time(0)
   , m_shutdown_time(0)
{
}

void Simulator::start()
{
   LOG_PRINT("In Simulator ctor.");

   m_config.logCoreMap();

   m_transport = Transport::create();
   m_hooks_manager = new HooksManager();
   m_dvfs_manager = new DvfsManager();
   m_core_manager = new CoreManager();
   m_thread_manager = new ThreadManager(m_core_manager);
   m_perf_counter_manager = new PerfCounterManager(m_thread_manager);
   m_sim_thread_manager = new SimThreadManager();
   m_clock_skew_minimization_manager = ClockSkewMinimizationManager::create(getCfg()->getString("clock_skew_minimization/scheme","none"));

   if (Sim()->getCfg()->getBool("traceinput/enabled", false))
      m_trace_manager = new TraceManager();
   else
      m_trace_manager = NULL;

   Fxsupport::init();

   PthreadEmu::init();

   m_hooks_manager->init();

   startMCP();

   m_sim_thread_manager->spawnSimThreads();

   m_lcp = new LCP();
   m_lcp_thread = Thread::create(m_lcp);
   m_lcp_thread->run();

   Instruction::initializeStaticInstructionModel();

   InstMode::inst_mode_init = InstMode::fromString(getCfg()->getString("general/inst_mode_init", "cache_only"));
   InstMode::inst_mode_roi  = InstMode::fromString(getCfg()->getString("general/inst_mode_roi",  "detailed"));
   InstMode::inst_mode_end  = InstMode::fromString(getCfg()->getString("general/inst_mode_end",  "fast_forward"));
   setInstrumentationMode(InstMode::inst_mode_init);

   if (m_config.getCurrentProcessNum() == 0) {
      /* Save a copy of the configuration for reference */
      m_config_file->saveAs(m_config.formatOutputFileName("sim.cfg"));
   }

// PIN_SpawnInternalThread doesn't schedule its threads until after PIN_StartProgram
//   m_transport->barrier();

   m_hooks_manager->callHooks(HookType::HOOK_SIM_START, 0);
   m_stats_manager->recordStats("start");
}

Simulator::~Simulator()
{
   // Done with all the Pin stuff, allow using Config::Config again
   m_config_file_allowed = true;

   m_stats_manager->recordStats("stop");
   m_hooks_manager->callHooks(HookType::HOOK_SIM_END, 0);

   TotalTimer::reports();

   m_shutdown_time = getTime();

   LOG_PRINT("Simulator dtor starting...");

   if ((m_config.getCurrentProcessNum() == 0) && \
      (m_config.getSimulationMode() == Config::FULL))
      m_thread_manager->terminateThreadSpawners();

   broadcastFinish();

   endMCP();

   m_hooks_manager->fini();

   if (m_clock_skew_minimization_manager)
      delete m_clock_skew_minimization_manager;

   m_sim_thread_manager->quitSimThreads();

   m_transport->barrier();

   m_lcp->finish();

   #if 0
   if (Config::getSingleton()->getCurrentProcessNum() == 0)
   {
      std::ofstream os(Config::getSingleton()->getOutputFileName().c_str());

      os << "Simulation timers: " << std::endl
         << "start time\t" << (m_start_time - m_boot_time) << std::endl
         << "stop time\t" << (m_stop_time - m_boot_time) << std::endl
         << "shutdown time\t" << (m_shutdown_time - m_boot_time) << std::endl;

      m_core_manager->outputSummary(os);
      os.close();
   }
   else
   {
      std::stringstream temp;
      m_core_manager->outputSummary(temp);
      assert(temp.str().length() == 0);
   }
   #endif

   delete m_trace_manager;
   delete m_lcp_thread;
   delete m_mcp_thread;
   delete m_lcp;
   delete m_mcp;
   delete m_sim_thread_manager;
   delete m_perf_counter_manager;
   delete m_thread_manager;
   delete m_core_manager;
   delete m_transport;
}

void Simulator::startTimer()
{
   m_start_time = getTime();
}

void Simulator::stopTimer()
{
   m_stop_time = getTime();
}

void Simulator::broadcastFinish()
{
   if (Config::getSingleton()->getCurrentProcessNum() != 0)
      return;

   m_num_procs_finished = 1;

   // let the rest of the simulator know its time to exit
   Transport::Node *globalNode = Transport::getSingleton()->getGlobalNode();

   SInt32 msg = LCP_MESSAGE_SIMULATOR_FINISHED;
   for (UInt32 i = 1; i < Config::getSingleton()->getProcessCount(); i++)
   {
      globalNode->globalSend(i, &msg, sizeof(msg));
   }

   while (m_num_procs_finished < Config::getSingleton()->getProcessCount())
   {
      sched_yield();
   }
}

void Simulator::handleFinish()
{
   LOG_ASSERT_ERROR(Config::getSingleton()->getCurrentProcessNum() != 0,
                    "LCP_MESSAGE_SIMULATOR_FINISHED received on master process.");

   Transport::Node *globalNode = Transport::getSingleton()->getGlobalNode();
   SInt32 msg = LCP_MESSAGE_SIMULATOR_FINISHED_ACK;
   globalNode->globalSend(0, &msg, sizeof(msg));

   m_finished = true;
}

void Simulator::deallocateProcess()
{
   LOG_ASSERT_ERROR(Config::getSingleton()->getCurrentProcessNum() == 0,
                    "LCP_MESSAGE_SIMULATOR_FINISHED_ACK received on slave process.");

   ++m_num_procs_finished;
}

void Simulator::startMCP()
{
   if (m_config.getCurrentProcessNum() != m_config.getProcessNumForCore(Config::getSingleton()->getMCPCoreNum()))
      return;

   LOG_PRINT("Creating new MCP object in process %i", m_config.getCurrentProcessNum());

   // FIXME: Can't the MCP look up its network itself in the
   // constructor?
   Core * mcp_core = m_core_manager->getCoreFromID(m_config.getMCPCoreNum());
   LOG_ASSERT_ERROR(mcp_core, "Could not find the MCP's core!");

   Network & mcp_network = *(mcp_core->getNetwork());
   m_mcp = new MCP(mcp_network);

   m_mcp_thread = Thread::create(m_mcp);
   m_mcp_thread->run();
}

void Simulator::endMCP()
{
   if (m_config.getCurrentProcessNum() == m_config.getProcessNumForCore(m_config.getMCPCoreNum()))
      m_mcp->finish();
}

void Simulator::ValidateDataUpdate(IntPtr address, Byte* data_buf, UInt32 data_length)
{
   if (Sim()->getConfig()->getSimulationMode() == Config::FULL)
   {
      ScopedLock sl(m_memory_lock);
      for(IntPtr i = 0; i < data_length; ++i)
         m_memory[address + i] = data_buf[i];
   }
}

bool Simulator::ValidateDataRead(IntPtr address, Byte* data_buf, UInt32 data_length)
{
   if (Sim()->getConfig()->getSimulationMode() == Config::FULL)
   {
      ScopedLock sl(m_memory_lock);
      for(IntPtr i = 0; i < data_length; ++i) {
         IntPtr a = address + i;
         if (m_memory.count(a) == 0)
            /* Never accessed before by us (probably initialized when loading the executable), assume correct and remember */
            m_memory[a] = data_buf[i];
         else if (m_memory[a] != data_buf[i])
            return false;//LOG_ASSERT_ERROR(m_memory[a] == data_buf[i], "Data corruption @ %x: read %02x, should be %02x", address, data_buf[i], m_memory[a]);
      }
   }
   return true;
}

bool Simulator::finished()
{
   return m_finished;
}

void Simulator::enablePerformanceModelsInCurrentProcess()
{
   Sim()->startTimer();
   for (UInt32 i = 0; i < Sim()->getConfig()->getNumLocalCores(); i++)
      Sim()->getCoreManager()->getCoreFromIndex(i)->enablePerformanceModels();
}

void Simulator::disablePerformanceModelsInCurrentProcess()
{
   Sim()->stopTimer();
   for (UInt32 i = 0; i < Sim()->getConfig()->getNumLocalCores(); i++)
      Sim()->getCoreManager()->getCoreFromIndex(i)->disablePerformanceModels();
}

void Simulator::setInstrumentationMode(InstMode::inst_mode_t new_mode)
{
   if (Sim()->getConfig()->getSimulationMode() == Config::LITE || Sim()->getConfig()->getSimulationMode() == Config::FULL)
      InstMode::SetInstrumentationMode(new_mode);
}
