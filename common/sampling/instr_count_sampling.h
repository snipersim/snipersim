#ifndef __INSTR_COUNT_SAMPLING
#define __INSTR_COUNT_SAMPLING

#include "sampling_provider.h"

class InstrCountSampling : public SamplingProvider
{
public:
   virtual void startSampling(SubsecondTime until)
   {}
   virtual InstrumentLevel::Level requestedInstrumentation()
   {
      return InstrumentLevel::INSTR;
   }
};

#endif /* __INSTR_COUNT_SAMPLING */
