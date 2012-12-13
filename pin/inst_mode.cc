#include "inst_mode.h"
#include "inst_mode_macros.h"
#include "pin.H"

void
InstMode::updateInstrumentationMode()
{
   PIN_RemoveInstrumentation();
}
