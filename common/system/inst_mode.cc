#include "log.h"
#include "inst_mode.h"

const char * inst_mode_names[] = {
   "INVALID", "DETAILED", "CACHE_ONLY", "FAST_FORWARD"
};

// Instrumentation modes
InstMode::inst_mode_t InstMode::inst_mode_init = InstMode::INVALID;
InstMode::inst_mode_t InstMode::inst_mode_roi  = InstMode::INVALID;
InstMode::inst_mode_t InstMode::inst_mode_end  = InstMode::INVALID;

// Initial instrumentation mode
InstMode::inst_mode_t InstMode::inst_mode = InstMode::INVALID;


__attribute__((weak)) void
InstMode::updateInstrumentationMode()
{
   LOG_PRINT_ERROR("%s: This version of this function should not be called", __FUNCTION__);
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
      LOG_PRINT_ERROR("Invalid instrumentation mode %s", str.c_str());
}
