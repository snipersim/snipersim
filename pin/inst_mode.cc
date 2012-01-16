#include "inst_mode.h"
#include "inst_mode_macros.h"
#include "instruction_modeling.h"
#include "simulator.h"
#include "log.h"
#include "local_storage.h"
#include "pin.H"

// Instrumentation modes
InstMode::inst_mode_t InstMode::inst_mode_init = InstMode::CACHE_ONLY;   // Change this into FAST_FORWARD if you don't care about warm caches
InstMode::inst_mode_t InstMode::inst_mode_roi  = InstMode::DETAILED;
InstMode::inst_mode_t InstMode::inst_mode_end  = InstMode::FAST_FORWARD;

// Initial instrumentation mode
InstMode::inst_mode_t InstMode::inst_mode = InstMode::inst_mode_init;


const char * inst_mode_names[] = {
   "INVALID", "DETAILED", "CACHE_ONLY", "FAST_FORWARD"
};

void
InstMode::SetInstrumentationMode(InstMode::inst_mode_t new_mode)
{
   if (new_mode != InstMode::inst_mode) {
      GetVmLock();
      InstMode::inst_mode = new_mode;
      printf("[SNIPER] Setting instrumentation mode to %s\n", inst_mode_names[new_mode]); fflush(stdout);
      PIN_RemoveInstrumentation();
      ReleaseVmLock();
   }
}

InstMode::inst_mode_t
InstMode::fromString(const String str)
{
   if (str == "cache_only")
      return CACHE_ONLY;
   else if (str == "detailed")
      return DETAILED;
   else if (str == "fast_forward")
      return FAST_FORWARD;
   else
      LOG_ASSERT_ERROR(false, "Invalid instrumentation mode %s", str.c_str());
}
