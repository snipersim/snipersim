#include "emulation.h"
#include "globals.h"
#include "threads.h"

#include <pin.H>

static void insCallback(INS ins, VOID *v)
{
}

void initEmulation()
{
   INS_AddInstrumentFunction(insCallback, 0);
}
