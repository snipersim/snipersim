#include "simulator.h"
#include "config.h"
#include "memory_manager_base.h"
#include "parametric_dram_directory_msi/memory_manager.h"
#include "log.h"
#include "config.hpp"

MemoryManagerBase*
MemoryManagerBase::createMMU(String protocol_type,
      Core* core, Network* network, ShmemPerfModel* shmem_perf_model)
{
   CachingProtocol_t caching_protocol = parseProtocolType(protocol_type);

   switch (caching_protocol)
   {
      case PARAMETRIC_DRAM_DIRECTORY_MSI:
         return new ParametricDramDirectoryMSI::MemoryManager(core, network, shmem_perf_model);

      default:
         LOG_PRINT_ERROR("Unsupported Caching Protocol (%u)", caching_protocol);
         return NULL;
   }
}

MemoryManagerBase::CachingProtocol_t
MemoryManagerBase::parseProtocolType(String& protocol_type)
{
   if (protocol_type == "parametric_dram_directory_msi")
      return PARAMETRIC_DRAM_DIRECTORY_MSI;
   else
      return NUM_CACHING_PROTOCOL_TYPES;
}

void MemoryManagerNetworkCallback(void* obj, NetPacket packet)
{
   MemoryManagerBase *mm = (MemoryManagerBase*) obj;
   assert(mm != NULL);

   switch (packet.type)
   {
      case SHARED_MEM_1:
         mm->handleMsgFromNetwork(packet);
         break;

      default:
         LOG_PRINT_ERROR("Got unrecognized packet type(%u)", packet.type);
         break;
   }
}

std::vector<core_id_t>
MemoryManagerBase::getCoreListWithMemoryControllers()
{
   SInt32 num_memory_controllers = -1;
   SInt32 memory_controllers_interleaving = 0;
   String memory_controller_positions_from_cfg_file = "";

   SInt32 core_count;

   core_count = Config::getSingleton()->getApplicationCores();
   try
   {
      num_memory_controllers = Sim()->getCfg()->getInt("perf_model/dram/num_controllers");
      UInt32 smt_cores = Sim()->getCfg()->getInt("perf_model/core/logical_cpus");
      memory_controllers_interleaving = Sim()->getCfg()->getInt("perf_model/dram/controllers_interleaving") * smt_cores;
      memory_controller_positions_from_cfg_file = Sim()->getCfg()->getString("perf_model/dram/controller_positions");
   }
   catch (...)
   {
      LOG_PRINT_ERROR("Error reading number of memory controllers or controller positions");
   }

   LOG_ASSERT_ERROR(num_memory_controllers <= core_count, "Num Memory Controllers(%i), Num Cores(%i)",
         num_memory_controllers, core_count);

   if (num_memory_controllers != -1)
   {
      std::vector<core_id_t> core_list_from_cfg_file;
      parseMemoryControllerList(memory_controller_positions_from_cfg_file, core_list_from_cfg_file, core_count);

      LOG_ASSERT_ERROR((core_list_from_cfg_file.size() == 0) || (core_list_from_cfg_file.size() == (size_t) num_memory_controllers),
            "num_memory_controllers(%i), num_controller_positions specified(%i)",
            num_memory_controllers, core_list_from_cfg_file.size());

      if (core_list_from_cfg_file.size() > 0)
      {
         // Return what we read from the config file
         return core_list_from_cfg_file;
      }
      else
      {
         UInt32 l_models_memory_1 = 0;
         try
         {
            config::Config *cfg = Sim()->getCfg();
            l_models_memory_1 = NetworkModel::parseNetworkType(cfg->getString("network/memory_model_1"));
         }
         catch (...)
         {
            LOG_PRINT_ERROR("Exception while reading network model types.");
         }

         std::pair<bool, std::vector<core_id_t> > core_list_with_memory_controllers_1 = NetworkModel::computeMemoryControllerPositions(l_models_memory_1, num_memory_controllers, core_count);
         return core_list_with_memory_controllers_1.second;
      }
   }
   else
   {
      std::vector<core_id_t> core_list_with_memory_controllers;

      if (memory_controllers_interleaving)
      {
         num_memory_controllers = (core_count + memory_controllers_interleaving - 1) / memory_controllers_interleaving; // Round up
         for (core_id_t i = 0; i < num_memory_controllers; i++)
         {
            assert((i*memory_controllers_interleaving) < core_count);
            core_list_with_memory_controllers.push_back(i * memory_controllers_interleaving);
         }
      }
      else
      {
         // All cores have memory controllers
         for (core_id_t i = 0; i < core_count; i++)
            core_list_with_memory_controllers.push_back(i);
      }

      return core_list_with_memory_controllers;
   }
}

void
MemoryManagerBase::parseMemoryControllerList(String& memory_controller_positions, std::vector<core_id_t>& core_list_from_cfg_file, SInt32 core_count)
{
   if (memory_controller_positions == "")
      return;

   size_t i = 0;
   bool end_reached = false;

   while(!end_reached)
   {
      size_t position = memory_controller_positions.find(',', i);
      core_id_t core_num;

      if (position != String::npos)
      {
         // The end of the string has not been reached
         String core_num_str = memory_controller_positions.substr(i, position-i);
         core_num = atoi(core_num_str.c_str());
      }
      else
      {
         // The end of the string has been reached
         String core_num_str = memory_controller_positions.substr(i);
         core_num = atoi(core_num_str.c_str());
         end_reached = true;
      }

      LOG_ASSERT_ERROR(core_num < core_count, "core_num(%i), num_cores(%i)", core_num, core_count);
      core_list_from_cfg_file.push_back(core_num);

      i = position + 1;
   }
}

void
MemoryManagerBase::printCoreListWithMemoryControllers(std::vector<core_id_t>& core_list_with_memory_controllers)
{
   std::ostringstream core_list;
   for (std::vector<core_id_t>::iterator it = core_list_with_memory_controllers.begin(); it != core_list_with_memory_controllers.end(); it++)
   {
      core_list << *it << " ";
   }
   fprintf(stderr, "Core IDs' with memory controllers = (%s)\n", (core_list.str()).c_str());
}
