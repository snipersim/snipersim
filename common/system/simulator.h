#ifndef SIMULATOR_H
#define SIMULATOR_H

#include "config.h"
#include "log.h"
#include "inst_mode.h"


class MCP;
class LCP;
class StatsManager;
class Transport;
class CoreManager;
class Thread;
class ThreadManager;
class PerfCounterManager;
class SimThreadManager;
class HooksManager;
class ClockSkewMinimizationManager;
class TraceManager;
class DvfsManager;
namespace config { class Config; }

class Simulator
{
public:
   Simulator();
   ~Simulator();

   void start();

   static Simulator* getSingleton() { return m_singleton; }
   static void setConfig(config::Config * cfg, Config::SimulationMode mode = Config::SimulationMode::FROM_CONFIG);
   static void allocate();
   static void release();

   MCP *getMCP() { return m_mcp; }
   LCP *getLCP() { return m_lcp; }
   CoreManager *getCoreManager() { return m_core_manager; }
   SimThreadManager *getSimThreadManager() { return m_sim_thread_manager; }
   ThreadManager *getThreadManager() { return m_thread_manager; }
   PerfCounterManager *getPerfCounterManager() { return m_perf_counter_manager; }
   ClockSkewMinimizationManager *getClockSkewMinimizationManager() { return m_clock_skew_minimization_manager; }
   Config *getConfig() { return &m_config; }
   config::Config *getCfg() {
      if (! m_config_file_allowed)
         fprintf(stderr, "getCfg() called after init, this is not nice\n");
      return m_config_file;
   }
   void hideCfg() { m_config_file_allowed = false; }
   StatsManager *getStatsManager() { return m_stats_manager; }
   DvfsManager *getDvfsManager() { return m_dvfs_manager; }
   HooksManager *getHooksManager() { return m_hooks_manager; }
   TraceManager *getTraceManager() { return m_trace_manager; }

   void ValidateDataUpdate(IntPtr address, Byte* data_buf, UInt32 data_length);
   bool ValidateDataRead(IntPtr address, Byte* data_buf, UInt32 data_length);

   static void enablePerformanceModelsInCurrentProcess();
   static void disablePerformanceModelsInCurrentProcess();

   void setInstrumentationMode(InstMode::inst_mode_t new_mode);
   InstMode::inst_mode_t getInstrumentationMode() { return InstMode::inst_mode; }

   void startTimer();
   void stopTimer();
   bool finished();

private:

   void startMCP();
   void endMCP();

   // handle synchronization of shutdown for distributed simulator objects
   void broadcastFinish();
   void handleFinish(); // slave processes
   void deallocateProcess(); // master process
   friend class LCP;

   MCP *m_mcp;
   Thread *m_mcp_thread;

   LCP *m_lcp;
   Thread *m_lcp_thread;

   Config m_config;
   Log m_log;
   StatsManager *m_stats_manager;
   Transport *m_transport;
   CoreManager *m_core_manager;
   ThreadManager *m_thread_manager;
   PerfCounterManager *m_perf_counter_manager;
   SimThreadManager *m_sim_thread_manager;
   ClockSkewMinimizationManager *m_clock_skew_minimization_manager;
   TraceManager *m_trace_manager;
   DvfsManager *m_dvfs_manager;
   HooksManager *m_hooks_manager;

   static Simulator *m_singleton;

   std::map<IntPtr, Byte> m_memory;
   Lock m_memory_lock;

   bool m_finished;
   UInt32 m_num_procs_finished;

   UInt64 m_boot_time;
   UInt64 m_start_time;
   UInt64 m_stop_time;
   UInt64 m_shutdown_time;

   static config::Config *m_config_file;
   static bool m_config_file_allowed;
   static Config::SimulationMode m_mode;
};

__attribute__((unused)) static Simulator *Sim()
{
   return Simulator::getSingleton();
}

#endif // SIMULATOR_H
