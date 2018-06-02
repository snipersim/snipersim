#include "core_model.h"
#include "core_model_nehalem.h"
#include "core_model_boom_v1.h"
#include "log.h"

std::map<String, const CoreModel*> CoreModel::s_core_models;

const CoreModel* CoreModel::getCoreModel(String type)
{
   if (!s_core_models.count(type))
   {
      if (type == "nehalem")
         s_core_models[type] = new CoreModelNehalem();
      else if (type == "riscv")
         s_core_models[type] = new CoreModelBoomV1();   
      else
         LOG_PRINT_ERROR("Unknown core model %s", type.c_str());
   }
   return s_core_models[type];
}
