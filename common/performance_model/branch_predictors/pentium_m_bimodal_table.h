#ifndef PENTIUM_M_BIMODAL_TABLE
#define PENTIUM_M_BIMODAL_TABLE

#include "simple_bimodal_table.h"

class PentiumMBimodalTable
   : public SimpleBimodalTable
{

public:

   // The Pentium M Global Branch Predictor
   // 512-entries
   // 6 tag bits per entry
   // 4-way set associative
   PentiumMBimodalTable()
      : SimpleBimodalTable(4096)
   {}

};

#endif /* PENTIUM_M_BIMODAL_TABLE */
