#include "config.h"

#include "network_model.h"
#include "network_types.h"
#include "packet_type.h"
#include "simulator.h"
#include "utils.h"
#include "config.hpp"

#include <sstream>
#include "log.h"

#define DEBUG

String Config::m_knob_output_directory;
UInt32 Config::m_knob_total_cores;
UInt32 Config::m_knob_num_process;
bool Config::m_knob_simarch_has_shared_mem;
String Config::m_knob_output_file;
bool Config::m_knob_enable_performance_modeling;
bool Config::m_knob_enable_dcache_modeling;
bool Config::m_knob_enable_icache_modeling;
bool Config::m_knob_use_magic;
bool Config::m_knob_enable_progress_trace;
bool Config::m_knob_enable_sync;
bool Config::m_knob_enable_sync_report;
bool Config::m_knob_osemu_pthread_replace;
UInt32 Config::m_knob_osemu_nprocs;
bool Config::m_knob_bbvs;

Config *Config::m_singleton;

Config *Config::getSingleton()
{
   assert(m_singleton != NULL);
   return m_singleton;
}

Config::Config()
      : m_current_process_num((UInt32)-1)
{
   // NOTE: We can NOT use logging in the config constructor! The log
   // has not been instantiated at this point!
   try
   {
      m_knob_output_directory = Sim()->getCfg()->getString("general/output_dir",".");
      m_knob_total_cores = Sim()->getCfg()->getInt("general/total_cores");
      m_knob_num_process = Sim()->getCfg()->getInt("general/num_processes");
      m_knob_simarch_has_shared_mem = Sim()->getCfg()->getBool("general/enable_shared_mem");
      m_knob_output_file = Sim()->getCfg()->getString("general/output_file");
      m_knob_enable_performance_modeling = Sim()->getCfg()->getBool("general/enable_performance_modeling");
      // TODO: these should be removed and queried directly from the cache
      m_knob_enable_dcache_modeling = Sim()->getCfg()->getBool("general/enable_dcache_modeling");
      m_knob_enable_icache_modeling = Sim()->getCfg()->getBool("general/enable_icache_modeling");

      m_knob_use_magic = Sim()->getCfg()->getBool("general/magic", false);
      m_knob_enable_progress_trace = Sim()->getCfg()->getBool("progress_trace/enabled", false);
      m_knob_enable_sync = Sim()->getCfg()->getString("clock_skew_minimization/scheme", "none") != "none";
      m_knob_enable_sync_report = Sim()->getCfg()->getBool("clock_skew_minimization/report", false);

      // Simulation Mode
      m_simulation_mode = parseSimulationMode(Sim()->getCfg()->getString("general/mode"));
      m_knob_bbvs = false; // No config setting here, but enabled by code (BBVSamplingProvider, [py|lua]_bbv) that needs it

      // OS Emulation flags
      m_knob_osemu_pthread_replace = Sim()->getCfg()->getBool("osemu/pthread_replace", true);
      m_knob_osemu_nprocs = Sim()->getCfg()->getInt("osemu/nprocs", 0);
   }
   catch(...)
   {
      fprintf(stderr, "ERROR: Config obtained a bad value from config.\n");
      exit(EXIT_FAILURE);
   }

   m_num_processes = m_knob_num_process;
   m_total_cores = m_knob_total_cores;

   if ((m_simulation_mode == LITE) && (m_num_processes > 1))
   {
      fprintf(stderr, "ERROR: Use only 1 process in lite mode\n");
      exit(EXIT_FAILURE);
   }

   m_singleton = this;

   assert(m_num_processes > 0);
   assert(m_total_cores > 0);

   // Add one for the MCP
   m_total_cores += 1;

   // Add the thread-spawners (one for each process)
   if (m_simulation_mode == FULL)
      m_total_cores += m_num_processes;

   // Adjust the number of cores corresponding to the network model we use
   m_total_cores = getNearestAcceptableCoreCount(m_total_cores);

   m_core_id_length = computeCoreIDLength(m_total_cores);

   GenerateCoreMap();
}

Config::~Config()
{
   // Clean up the dynamic memory we allocated
   delete [] m_proc_to_core_list_map;
}

UInt32 Config::getTotalCores()
{
   return m_total_cores;
}

UInt32 Config::getApplicationCores()
{
   if (m_simulation_mode == FULL)
      return (getTotalCores() - (1 + getProcessCount()));
   else
      return (getTotalCores() - 1);
}

UInt32 Config::getNumLocalApplicationCores()
{
   if (m_simulation_mode == FULL)
      return getNumCoresInProcess(getCurrentProcessNum()) - 1 - (getCurrentProcessNum() == 0 ? 1 : 0);
   else
      return getTotalCores() - 1;
}

core_id_t Config::getThreadSpawnerCoreNum(UInt32 proc_num)
{
   if (m_simulation_mode == FULL)
      return (getTotalCores() - (1 + getProcessCount() - proc_num));
   else
      return INVALID_CORE_ID;
}

core_id_t Config::getCurrentThreadSpawnerCoreNum()
{
   if (m_simulation_mode == FULL)
      return (getTotalCores() - (1 + getProcessCount() - getCurrentProcessNum()));
   else
      return INVALID_CORE_ID;
}

UInt32 Config::computeCoreIDLength(UInt32 core_count)
{
   UInt32 num_bits = ceilLog2(core_count);
   if ((num_bits % 8) == 0)
      return (num_bits / 8);
   else
      return (num_bits / 8) + 1;
}

void Config::GenerateCoreMap()
{
   m_proc_to_core_list_map = new CoreList[m_num_processes];
   m_core_to_proc_map.resize(m_total_cores);

   // Shared caches need to run on the same core, so don't stripe as in stock Graphite
   UInt32 cores_per_proc = (getApplicationCores() + m_num_processes - 1) / m_num_processes; // Round up
   for (UInt32 i = 0; i < (m_total_cores - m_num_processes - 1) ; i++)
   {
      UInt32 proc_num = (i / cores_per_proc) % m_num_processes; // Do % just to be sure, getNearestAcceptableCoreCount may have increased the total number of cores beyond what we would expect
      m_core_to_proc_map [i] = proc_num;
      m_proc_to_core_list_map[proc_num].push_back(i);
   }

   // Assign the thread-spawners to cores
   // Thread-spawners occupy core-id's (m_total_cores - m_num_processes - 1) to (m_total_cores - 2)
   UInt32 current_proc = 0;
   for (UInt32 i = (m_total_cores - m_num_processes - 1); i < (m_total_cores - 1); i++)
   {
      m_core_to_proc_map[i] = current_proc;
      m_proc_to_core_list_map[current_proc].push_back(i);
      current_proc++;
      current_proc %= m_num_processes;
   }

   // Add one for the MCP, runs in process 0
   m_proc_to_core_list_map[0].push_back(m_total_cores - 1);
   m_core_to_proc_map[m_total_cores - 1] = 0;
}

void Config::logCoreMap()
{
   // Log the map we just created
   LOG_PRINT("Process num: %d\n", m_num_processes);
   for (UInt32 i=0; i < m_num_processes; i++)
   {
      LOG_ASSERT_ERROR(!m_proc_to_core_list_map[i].empty(),
                       "Process %u assigned zero cores.", i);

      std::stringstream ss;
      ss << "Process " << i << ": (" << m_proc_to_core_list_map[i].size() << ") ";
      for (CLCI m = m_proc_to_core_list_map[i].begin(); m != m_proc_to_core_list_map[i].end(); m++)
         ss << "[" << *m << "]";
      LOG_PRINT(ss.str().c_str());
   }
}

SInt32 Config::getIndexFromCoreID(UInt32 proc_num, core_id_t core_id)
{
   CoreList core_list = getCoreListForProcess(proc_num);
   for (UInt32 i = 0; i < core_list.size(); i++)
   {
      if (core_list[i] == core_id)
         return (SInt32) i;
   }
   return -1;
}

core_id_t Config::getCoreIDFromIndex(UInt32 proc_num, SInt32 index)
{
   CoreList core_list = getCoreListForProcess(proc_num);
   if (index < ((SInt32) core_list.size()))
   {
      return core_list[index];
   }
   else
   {
      return -1;
   }
}

// Parse XML config file and use it to fill in config state.  Only modifies
// fields specified in the config file.  Therefore, this method can be used
// to override only the specific options given in the file.
void Config::loadFromFile(char* filename)
{
   return;
}

// Fill in config state from command-line arguments.  Only modifies fields
// specified on the command line.  Therefore, this method can be used to
// override only the specific options given.
void Config::loadFromCmdLine()
{
   return;
}

bool Config::isSimulatingSharedMemory() const
{
   return (bool)m_knob_simarch_has_shared_mem;
}

bool Config::getEnablePerformanceModeling() const
{
   return (bool)m_knob_enable_performance_modeling;
}

bool Config::getEnableDCacheModeling() const
{
   return (bool)m_knob_enable_dcache_modeling;
}

bool Config::getEnableICacheModeling() const
{
   return (bool)m_knob_enable_icache_modeling;
}

String Config::getOutputFileName() const
{
   return formatOutputFileName(m_knob_output_file);
}

String Config::getOutputDirectory() const
{
   return m_knob_output_directory;
}

String Config::formatOutputFileName(String filename) const
{
   return m_knob_output_directory + "/" + filename;
}

void Config::updateCommToCoreMap(UInt32 comm_id, core_id_t core_id)
{
   m_comm_to_core_map[comm_id] = core_id;
}

UInt32 Config::getCoreFromCommId(UInt32 comm_id)
{
   CommToCoreMap::iterator it = m_comm_to_core_map.find(comm_id);
   return it == m_comm_to_core_map.end() ? INVALID_CORE_ID : it->second;
}

Config::SimulationMode Config::parseSimulationMode(String mode)
{
   if (mode == "full")
      return FULL;
   else if (mode == "lite")
      return LITE;
   else
      LOG_PRINT_ERROR("Unrecognized Simulation Mode(%s)", mode.c_str());

   return NUM_SIMULATION_MODES;
}

void Config::getNetworkModels(UInt32 *models) const
{
   try
   {
      config::Config *cfg = Sim()->getCfg();
      models[STATIC_NETWORK_USER_1] = NetworkModel::parseNetworkType(cfg->getString("network/user_model_1"));
      models[STATIC_NETWORK_USER_2] = NetworkModel::parseNetworkType(cfg->getString("network/user_model_2"));
      models[STATIC_NETWORK_MEMORY_1] = NetworkModel::parseNetworkType(cfg->getString("network/memory_model_1"));
      models[STATIC_NETWORK_MEMORY_2] = NetworkModel::parseNetworkType(cfg->getString("network/memory_model_2"));
      models[STATIC_NETWORK_SYSTEM] = NetworkModel::parseNetworkType(cfg->getString("network/system_model"));
   }
   catch (...)
   {
      LOG_PRINT_ERROR("Exception while reading network model types.");
   }
}

UInt32 Config::getNearestAcceptableCoreCount(UInt32 core_count)
{
   UInt32 nearest_acceptable_core_count = 0;

   UInt32 l_models[NUM_STATIC_NETWORKS];
   try
   {
      config::Config *cfg = Sim()->getCfg();
      l_models[STATIC_NETWORK_USER_1] = NetworkModel::parseNetworkType(cfg->getString("network/user_model_1"));
      l_models[STATIC_NETWORK_USER_2] = NetworkModel::parseNetworkType(cfg->getString("network/user_model_2"));
      l_models[STATIC_NETWORK_MEMORY_1] = NetworkModel::parseNetworkType(cfg->getString("network/memory_model_1"));
      l_models[STATIC_NETWORK_MEMORY_2] = NetworkModel::parseNetworkType(cfg->getString("network/memory_model_2"));
      l_models[STATIC_NETWORK_SYSTEM] = NetworkModel::parseNetworkType(cfg->getString("network/system_model"));
   }
   catch (...)
   {
      LOG_PRINT_ERROR("Exception while reading network model types.");
   }

   for (UInt32 i = 0; i < NUM_STATIC_NETWORKS; i++)
   {
      std::pair<bool,SInt32> core_count_constraints = NetworkModel::computeCoreCountConstraints(l_models[i], (SInt32) core_count);
      if (core_count_constraints.first)
      {
         // Network Model has core count constraints
         if ((nearest_acceptable_core_count != 0) &&
             (core_count_constraints.second != (SInt32) nearest_acceptable_core_count))
         {
            LOG_PRINT_ERROR("Problem using the network models specified in the configuration file.");
         }
         else
         {
            nearest_acceptable_core_count = core_count_constraints.second;
         }
      }
   }

   if (nearest_acceptable_core_count == 0)
      nearest_acceptable_core_count = core_count;

   return nearest_acceptable_core_count;
}
