#include "dram_directory_perf_model_base.h"
#include "dram_directory_perf_model.h"
#include "log.h"

DramDirectoryPerfModelBase* DramDirectoryPerfModelBase::createModel(UInt32 type)
{
   switch(type)
   {
      case(DRAM_DIRECTORY_PERF_MODEL):
         return new DramDirectoryPerfModel();
            
      default:
         LOG_ASSERT_ERROR(false, "Unsupported DramDirectoryModel type: %u", type);
         return NULL;
   }
}
