#include "sampling_provider.h"
#include "simulator.h"
#include "config.hpp"
#include "log.h"
#include "instr_count_sampling.h"

SamplingProvider*
SamplingProvider::create()
{
   String sampling_type = Sim()->getCfg()->getString("sampling/type");
   if (sampling_type == "instr_count")
   {
      return new InstrCountSampling();
   }
   else
   {
      LOG_PRINT_ERROR("Unexpected sampling type '%s'", sampling_type.c_str());
   }
}
