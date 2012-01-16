#ifndef PENTIUM_M_GLOBAL_PREDICTOR
#define PENTIUM_M_GLOBAL_PREDICTOR

#include "global_predictor.h"

class PentiumMGlobalPredictor
   : public GlobalPredictor
{

public:

   // The Pentium M Global Branch Predictor
   // 512-entries
   // 6 tag bits per entry
   // 4-way set associative
   PentiumMGlobalPredictor()
      : GlobalPredictor(2048, 6, 4)
   {}

};

#endif /* PENTIUM_M_GLOBAL_PREDICTOR */
