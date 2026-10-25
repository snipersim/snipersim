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
#include "simulator.h"
#include "config.hpp"

std::map<String, const CoreModel*> CoreModel::s_core_models;

const CoreModel* CoreModel::getCoreModel(String type)
{
   if (!s_core_models.count(type))
   {
      // The model's latency and port tables are indexed by its own
      // architecture's instruction ids; on a configuration of another
      // architecture the ids fall outside them and every instruction costs
      // a cycle, silently.  TraceThread checks the trace against the
      // decoder; this checks the model against the decoder's architecture.
      const String arch = Sim()->getCfg()->getString("general/arch");
      const bool matches = type == "nehalem" ? (arch == "intel" || arch == "x86")
                         : type == "riscv"   ? arch == "riscv"
                         : type.substr(0, 7) == "cortex-" ? (arch == "armv8" || arch == "arm")
                         : true;
      LOG_ASSERT_ERROR(matches, "core_model %s is a model of another architecture than general/arch = %s "
                                "(nehalem: intel; riscv: riscv; cortex-a53, cortex-a72: armv8)",
                                type.c_str(), arch.c_str());
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
