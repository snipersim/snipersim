#ifndef PENTIUM_M_LOOP_BRANCH_PREDICTOR
#define PENTIUM_M_LOOP_BRANCH_PREDICTOR

#include "lpb.h"

class PentiumMLoopBranchPredictor : public LoopBranchPredictor
{

public:

   // 128 entries
   // 6 bit tag
   // 2 ways
   PentiumMLoopBranchPredictor()
      : LoopBranchPredictor(128, 6, 2)
   {}

};

#endif /* PENTIUM_M_LOOP_BRANCH_PREDICTOR */
