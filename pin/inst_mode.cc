#include "inst_mode.h"
#include "inst_mode_macros.h"
#include "instruction_modeling.h"
#include "simulator.h"
#include "log.h"
#include "local_storage.h"
#include "pin.H"


const char * inst_mode_names[] = {
   "INVALID", "DETAILED", "CACHE_ONLY", "FAST_FORWARD"
};

void
InstMode::SetInstrumentationMode(InstMode::inst_mode_t new_mode)
{
   if (new_mode != InstMode::inst_mode) {
      InstMode::inst_mode = new_mode;
      printf("[SNIPER] Setting instrumentation mode to %s\n", inst_mode_names[new_mode]); fflush(stdout);
      PIN_RemoveInstrumentation();
   }
}
