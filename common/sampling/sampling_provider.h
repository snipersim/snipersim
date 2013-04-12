#ifndef __SAMPLING_PROVIDER
#define __SAMPLING_PROVIDER

#include "fixed_types.h"
#include "subsecond_time.h"

// Emulate 'enum class InstrumentLevel' because it is
// unsupported in GCC 4.4
namespace InstrumentLevel {
   enum Level {
      INSTR_WITH_BBVS,
      INSTR,
      NONE,
   };
};

class SamplingProvider
{
public:
   virtual ~SamplingProvider() {}
   virtual void startSampling(SubsecondTime until) = 0;
   virtual int32_t registerSignal()
   {
      // Do not register a signal
      return 0;
   }
   virtual InstrumentLevel::Level requestedInstrumentation() = 0;

   static SamplingProvider* create();
};

#endif /* __SAMPLING_PROVIDER */
