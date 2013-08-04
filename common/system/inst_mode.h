#ifndef INSTMODE_H
#define INSTMODE_H

#include "fixed_types.h"

class InstMode
{
   public:
      enum inst_mode_t {
         INVALID = 0, DETAILED, CACHE_ONLY, FAST_FORWARD
      };
      static inst_mode_t inst_mode_init, inst_mode_roi, inst_mode_end;
      static inst_mode_t fromString(const String str);

   private:
      static inst_mode_t inst_mode;
      static void updateInstrumentationMode();

      // Access through Sim()
      friend class Simulator;
};

extern const char * inst_mode_names[];

#endif // INSTMODE_H
