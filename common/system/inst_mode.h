#ifndef INSTMODE_H
#define INSTMODE_H

#include "fixed_types.h"

class InstMode
{
   public:
      enum inst_mode_t {
         BASE = 0, DETAILED, CACHE_ONLY, FAST_FORWARD
      };
      static inst_mode_t inst_mode_init, inst_mode_roi, inst_mode_end;
      static inst_mode_t fromString(const String str);

   private:
      static inst_mode_t inst_mode;
      static void SetInstrumentationMode(InstMode::inst_mode_t new_mode);

      // Access through Sim()
      friend class Simulator;
};

#endif // INSTMODE_H
