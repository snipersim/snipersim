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
      FULL = 0,
      LITE,
      STANDALONE,
      NUM_SIMULATION_MODES,
      FROM_CONFIG
   };

   typedef std::vector<UInt32> CoreToProcMap;
   typedef std::vector<core_id_t> CoreList;
   typedef std::vector<core_id_t>::const_iterator CLCI;
   typedef std::unordered_map<UInt32,core_id_t> CommToCoreMap;

   Config(SimulationMode mode);
   ~Config();

   void loadFromFile(char* filename);
   void loadFromCmdLine();

   // Return the number of processes involved in this simulation
   UInt32 getProcessCount() { return m_num_processes; }
   void setProcessCount(UInt32 in_num_processes) { m_num_processes = in_num_processes; }

   // Retrieve and set the process number for this process (I'm expecting
   //  that the initialization routine of the Transport layer will set this)
   UInt32 getCurrentProcessNum() { return m_current_process_num; }
   void setProcessNum(UInt32 in_my_proc_num) { m_current_process_num = in_my_proc_num; }

   core_id_t getMCPCoreNum() { return getTotalCores() -1; }

   core_id_t getMainThreadCoreNum() { return 0;}

   core_id_t getThreadSpawnerCoreNum(UInt32 proc_num);
   core_id_t getCurrentThreadSpawnerCoreNum();

   // Return the number of modules (cores) in a given process
   UInt32 getNumCoresInProcess(UInt32 proc_num)
   { assert(proc_num < m_num_processes); return m_proc_to_core_list_map[proc_num].size(); }

   SInt32 getIndexFromCoreID(UInt32 proc_num, core_id_t core_id);
   core_id_t getCoreIDFromIndex(UInt32 proc_num, SInt32 index);

   // Return total number of cores in this process
   UInt32 getNumLocalCores() { return getNumCoresInProcess(getCurrentProcessNum()); }
   // Return number of application cores in this process (subtract LCP in all processes, MCP for process 0)
   UInt32 getNumLocalApplicationCores();

   // Return the total number of modules in all processes
   UInt32 getTotalCores();
   UInt32 getApplicationCores();

   // Return an array of core numbers for a given process
   //  The returned array will have numMods(proc_num) elements
   const CoreList & getCoreListForProcess(UInt32 proc_num)
   { assert(proc_num < m_num_processes); return m_proc_to_core_list_map[proc_num]; }

   const CoreList & getCoreListForCurrentProcess()
   { return getCoreListForProcess(getCurrentProcessNum()); }

   UInt32 getProcessNumForCore(UInt32 core)
   { assert(core < m_total_cores); return m_core_to_proc_map[core]; }

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
   bool isSimulatingSharedMemory() const;
   bool getEnablePerformanceModeling() const;
   bool getEnableDCacheModeling() const;
   bool getEnableICacheModeling() const;
   bool useMagic() const { return m_knob_use_magic; }
   bool getEnableProgressTrace() const { return m_knob_enable_progress_trace; }
   bool getEnableSync() const { return m_knob_enable_sync; }
   bool getEnableSyncReport() const { return m_knob_enable_sync_report; }
   bool getOSEmuPthreadReplace() const { return m_knob_osemu_pthread_replace; }
   UInt32 getOSEmuNprocs() const { return m_knob_osemu_nprocs; }
   bool getEnablePerBasicblock() const { return m_knob_enable_perbasicblock; }
   void setEnablePerBasicblock(bool enable) { m_knob_enable_perbasicblock = enable; }

   bool getBBVsEnabled() const { return m_knob_bbvs; }
   void setBBVsEnabled(bool enable) { m_knob_bbvs = enable; }

   // Logging
   String getOutputFileName() const;
   String getOutputDirectory() const;
   String formatOutputFileName(String filename) const;
   void logCoreMap();

   static Config *getSingleton();

private:
   void GenerateCoreMap();

   UInt32  m_num_processes;         // Total number of processes (incl myself)
   UInt32  m_total_cores;           // Total number of cores in all processes
   UInt32  m_core_id_length;        // Number of bytes needed to store a core_id

   UInt32  m_current_process_num;          // Process number for this process

   // This data structure keeps track of which cores are in each process.
   // It is an array of size num_processes where each element is a list of
   // core numbers.  Each list specifies which cores are in the corresponding
   // process.
   CoreToProcMap m_core_to_proc_map;
   CoreList* m_proc_to_core_list_map;

   CommToCoreMap m_comm_to_core_map;

   // Simulation Mode
   SimulationMode m_simulation_mode;

   UInt32  m_mcp_process;          // The process where the MCP lives

   static Config *m_singleton;

   static String m_knob_output_directory;
   static UInt32 m_knob_total_cores;
   static UInt32 m_knob_num_process;
   static bool m_knob_simarch_has_shared_mem;
   static String m_knob_output_file;
   static bool m_knob_enable_performance_modeling;
   static bool m_knob_enable_dcache_modeling;
   static bool m_knob_enable_icache_modeling;
   static bool m_knob_use_magic;
   static bool m_knob_enable_progress_trace;
   static bool m_knob_enable_sync;
   static bool m_knob_enable_sync_report;
   static bool m_knob_osemu_pthread_replace;
   static UInt32 m_knob_osemu_nprocs;
   static bool m_knob_bbvs;
   static bool m_knob_enable_perbasicblock;

   static SimulationMode parseSimulationMode(String mode);
   static UInt32 computeCoreIDLength(UInt32 core_count);
   static UInt32 getNearestAcceptableCoreCount(UInt32 core_count);
};

#endif
