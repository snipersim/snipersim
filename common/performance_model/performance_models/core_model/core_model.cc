#include "core_model.h"
#include "core_model_nehalem.h"
#if SNIPER_RISCV
# include "core_model_boom_v1.h"
#endif /* SNIPER_RISCV */
#if SNIPER_ARM
# include "core_model_cortex_a72.h"
# include "core_model_cortex_a53.h"
#endif /* SNIPER_ARM */
#include "log.h"

std::map<String, const CoreModel*> CoreModel::s_core_models;

const CoreModel* CoreModel::getCoreModel(String type)
{
   if (!s_core_models.count(type))
   {
      if (type == "nehalem")
         s_core_models[type] = new CoreModelNehalem();
#if SNIPER_RISCV
      else if (type == "riscv")
         s_core_models[type] = new CoreModelBoomV1();   
#endif /* SNIPER_RISCV */
#if SNIPER_ARM
      else if (type == "cortex-a72")
         s_core_models[type] = new CoreModelCortexA72();
      else if (type == "cortex-a53")
         s_core_models[type] = new CoreModelCortexA53();
#endif /* SNIPER_ARM */
      else
         LOG_PRINT_ERROR("Unknown core model %s", type.c_str());
   }
   return s_core_models[type];
}
