#ifndef PENTIUM_M_INDIRECT_BRANCH_TARGET_BUFFER
#define PENTIUM_M_INDIRECT_BRANCH_TARGET_BUFFER

#include "ibtb.h"

class PentiumMIndirectBranchTargetBuffer
   : public IndirectBranchTargetBuffer
{

public:

   // The Pentium M Indirect Branch Target Buffer (iBTB)
   // 256 entries
   // 7-bit tag
   PentiumMIndirectBranchTargetBuffer()
      : IndirectBranchTargetBuffer(256,7)
   {}

};

#endif /* PENTIUM_M_INDIRECT_BRANCH_TARGET_BUFFER */

