#include "simulator.h"
#include "nvm_perf_model.h"
#include "nvm_perf_model_constant.h"
#include "nvm_perf_model_readwrite.h"
#include "nvm_perf_model_normal.h"
#include "config.hpp"

NvmPerfModel* NvmPerfModel::createNvmPerfModel(core_id_t core_id, UInt32 cache_block_size)
{
   String type = Sim()->getCfg()->getString("perf_model/dram/type");

   if (type == "constant")
   {
      return new NvmPerfModelConstant(core_id, cache_block_size);
   }
   else if (type == "readwrite")
   {
      return new NvmPerfModelReadWrite(core_id, cache_block_size);
   }
   else if (type == "normal")
   {
      return new NvmPerfModelNormal(core_id, cache_block_size);
   }
   else
   {
      LOG_PRINT_ERROR("Invalid NVM model type %s", type.c_str());
   }
}

SubsecondTime NvmPerfModel::getReadLatency()
{
   float latency = Sim()->getCfg()->hasKey("perf_model/dram/read_latency") ? 
                   Sim()->getCfg()->getFloat("perf_model/dram/read_latency") : 
                   Sim()->getCfg()->getFloat("perf_model/dram/latency");

   return SubsecondTime::FS() * static_cast<uint64_t>(TimeConverter<float>::NStoFS(latency));
}

SubsecondTime NvmPerfModel::getWriteLatency()
{
   float latency = Sim()->getCfg()->hasKey("perf_model/dram/write_latency") ? 
                   Sim()->getCfg()->getFloat("perf_model/dram/write_latency") : 
                   Sim()->getCfg()->getFloat("perf_model/dram/latency");

   return SubsecondTime::FS() * static_cast<uint64_t>(TimeConverter<float>::NStoFS(latency));
}

SubsecondTime NvmPerfModel::getLogLatency()
{
   float latency = Sim()->getCfg()->hasKey("perf_model/dram/log_latency") ? 
                   Sim()->getCfg()->getFloat("perf_model/dram/log_latency") : 0;

   return SubsecondTime::FS() * static_cast<uint64_t>(TimeConverter<float>::NStoFS(latency));
}