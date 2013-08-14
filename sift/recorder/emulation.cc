#include "emulation.h"
#include "globals.h"
#include "threads.h"

#include <pin.H>

static void handleRdtsc(THREADID threadid, PIN_REGISTER * gax, PIN_REGISTER * gdx)
{
   Sift::EmuRequest req;
   Sift::EmuReply res;
   bool emulated = thread_data[threadid].output->Emulate(Sift::EmuTypeRdtsc, req, res);

   if (emulated)
   {
      // Return in eax and edx
      gdx->dword[0] = res.rdtsc.cycles >> 32;
      gax->dword[0] = res.rdtsc.cycles & 0xffffffff;
   }
}

static void insCallback(INS ins, VOID *v)
{
   if (INS_IsRDTSC(ins))
      INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR)handleRdtsc, IARG_THREAD_ID, IARG_REG_REFERENCE, REG_GAX, IARG_REG_REFERENCE, REG_GDX, IARG_END);
}

void initEmulation()
{
   INS_AddInstrumentFunction(insCallback, 0);
}
