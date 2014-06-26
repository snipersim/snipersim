// config.h
//
// The Config class is used to set, store, and retrieve all the configurable
// parameters of the simulator.
//
// Initial creation: Sep 18, 2008 by jasonm

#ifndef CONFIG_H
#define CONFIG_H

// Enable to run core performance model in separate thread
// When # simulated cores > # host cores, this is probably not very useful
//#define ENABLE_PERF_MODEL_OWN_THREAD

#include "fixed_types.h"
#include "clock_skew_minimization_object.h"
#include "cache_efficiency_tracker.h"

#include <vector>
#include <set>
#include <unordered_map>
#include <iostream>
#include <cassert>

struct NetworkModelAnalyticalParameters;

class Config
{
public:
   enum SimulationMode
   {
      PINTOOL = 0,
      STANDALONE,
      NUM_SIMULATION_MODES
   };

   enum SimulationROI
   {
      ROI_FULL,
      ROI_MAGIC,
      ROI_SCRIPT
   };

   typedef std::unordered_map<UInt32,core_id_t> CommToCoreMap;

   Config(SimulationMode mode);
   ~Config();

   void loadFromFile(char* filename);
   void loadFromCmdLine();

   // Return the total number of modules in all processes
   UInt32 getTotalCores();
   UInt32 getApplicationCores();

   // For mapping between user-land communication id's to actual core id's
   void updateCommToCoreMap(UInt32 comm_id, core_id_t core_id);
   UInt32 getCoreFromCommId(UInt32 comm_id);

   // Fills in an array with the models for each static network
   void getNetworkModels(UInt32 *) const;

   // Get CoreId length
   UInt32 getCoreIDLength()
   { return m_core_id_length; }

   SimulationMode getSimulationMode()
   { return m_simulation_mode; }

   // Knobs
   UInt32 getNumHostCores() const { return m_knob_num_host_cores; }
   bool getEnableSMCSupport() const { return m_knob_enable_smc_support; }
   void forceEnableSMCSupport() { m_knob_enable_smc_support = true; }
   bool getIssueMemopsAtFunctional() const { return m_knob_issue_memops_at_functional; }
   bool getEnableICacheModeling() const { return m_knob_enable_icache_modeling; }
   SimulationROI getSimulationROI() const { return m_knob_roi; }
   bool getEnableProgressTrace() const { return m_knob_enable_progress_trace; }
   bool getEnableSync() const { return m_knob_enable_sync; }
   bool getEnableSyncReport() const { return m_knob_enable_sync_report; }
   bool getOSEmuPthreadReplace() const { return m_knob_osemu_pthread_replace; }
   UInt32 getOSEmuNprocs() const { return m_knob_osemu_nprocs; }
   bool getOSEmuClockReplace() const { return m_knob_osemu_clock_replace; }
   time_t getOSEmuTimeStart() const { return m_knob_osemu_time_start; }
   ClockSkewMinimizationObject::Scheme getClockSkewMinimizationScheme() const { return m_knob_clock_skew_minimization_scheme; }
   UInt64 getHPIInstructionsPerCore() const { return m_knob_hpi_percore; }
   UInt64 getHPIInstructionsGlobal() const { return m_knob_hpi_global; }
   bool getEnableSpinLoopDetection() const { return m_knob_enable_spinloopdetection; }
   bool suppressStdout() const { return m_suppress_stdout; }
   bool suppressStderr() const { return m_suppress_stderr; }
   bool getEnablePinPlay() const { return m_knob_enable_pinplay; }
   bool getEnableSyscallEmulation() const { return m_knob_enable_syscall_emulation; }

   bool getBBVsEnabled() const { return m_knob_bbvs; }
   void setBBVsEnabled(bool enable) { m_knob_bbvs = enable; }

   const CacheEfficiencyTracker::Callbacks& getCacheEfficiencyCallbacks() const { return m_cache_efficiency_callbacks; }
   bool hasCacheEfficiencyCallbacks() const { return m_cache_efficiency_callbacks.notify_evict_func != NULL; }
   void setCacheEfficiencyCallbacks(CacheEfficiencyTracker::CallbackGetOwner get_owner_func, CacheEfficiencyTracker::CallbackNotifyAccess notify_access_func, CacheEfficiencyTracker::CallbackNotifyEvict notify_evict_func, UInt64 user_arg);

   // Logging
   String getOutputDirectory() const;
   String formatOutputFileName(String filename) const;
   void logCoreMap();
   bool getCircularLogEnabled() const { return m_circular_log_enabled; }

   static Config *getSingleton();

private:
   UInt32  m_total_cores;           // Total number of cores in all processes
   UInt32  m_core_id_length;        // Number of bytes needed to store a core_id

   CommToCoreMap m_comm_to_core_map;

   // Simulation Mode
   SimulationMode m_simulation_mode;

   static Config *m_singleton;

   static String m_knob_output_directory;
   static UInt32 m_knob_total_cores;
   static UInt32 m_knob_num_host_cores;
   static bool m_knob_enable_smc_support;
   static bool m_knob_issue_memops_at_functional;
   static bool m_knob_enable_icache_modeling;
   static SimulationROI m_knob_roi;
   static bool m_knob_enable_progress_trace;
   static bool m_knob_enable_sync;
   static bool m_knob_enable_sync_report;
   static bool m_knob_osemu_pthread_replace;
   static UInt32 m_knob_osemu_nprocs;
   static bool m_knob_osemu_clock_replace;
   static time_t m_knob_osemu_time_start;
   static bool m_knob_bbvs;
   static ClockSkewMinimizationObject::Scheme m_knob_clock_skew_minimization_scheme;
   static UInt64 m_knob_hpi_percore;
   static UInt64 m_knob_hpi_global;
   static bool m_knob_enable_spinloopdetection;
   static bool m_suppress_stdout;
   static bool m_suppress_stderr;
   static bool m_circular_log_enabled;
   static bool m_knob_enable_pinplay;
   static bool m_knob_enable_syscall_emulation;

   static CacheEfficiencyTracker::Callbacks m_cache_efficiency_callbacks;

   static SimulationMode parseSimulationMode(String mode);
   static UInt32 computeCoreIDLength(UInt32 core_count);
   static UInt32 getNearestAcceptableCoreCount(UInt32 core_count);
};

#endif
