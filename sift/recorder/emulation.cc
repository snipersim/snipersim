#include "emulation.h"
#include "sift_assert.h"
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

static ADDRINT emuGetNprocs(THREADID threadid)
{
   Sift::EmuRequest req;
   Sift::EmuReply res;
   bool emulated = thread_data[threadid].output->Emulate(Sift::EmuTypeGetProcInfo, req, res);

   sift_assert(emulated);
   return res.getprocinfo.emunprocs;
}

static ADDRINT emuGetCPU(THREADID threadid)
{
   Sift::EmuRequest req;
   Sift::EmuReply res;
   bool emulated = thread_data[threadid].output->Emulate(Sift::EmuTypeGetProcInfo, req, res);

   sift_assert(emulated);
   return res.getprocinfo.procid;
}

ADDRINT emuClockGettime(THREADID threadid, clockid_t clk_id, struct timespec *tp)
{
   switch(clk_id)
   {
      case CLOCK_REALTIME:
      case CLOCK_MONOTONIC:
         // Return simulated time
         if (tp)
         {
            Sift::EmuRequest req;
            Sift::EmuReply res;
            bool emulated = thread_data[threadid].output->Emulate(Sift::EmuTypeGetTime, req, res);

            sift_assert(emulated);
            tp->tv_sec = res.gettime.time_ns / 1000000000;
            tp->tv_nsec = res.gettime.time_ns % 1000000000;
         }
         return 0;
      default:
         // Unknown/non-emulated clock types (such as CLOCK_PROCESS_CPUTIME_ID/CLOCK_THREAD_CPUTIME_ID)
         return clock_gettime(clk_id, tp);
   }
}

ADDRINT emuGettimeofday(THREADID threadid, struct timeval *tv, struct timezone *tz)
{
   //sift_assert(tz == NULL); // gettimeofday() with non-NULL timezone not supported
   sift_assert(tv != NULL); // gettimeofday() called with NULL timeval not supported

   Sift::EmuRequest req;
   Sift::EmuReply res;
   bool emulated = thread_data[threadid].output->Emulate(Sift::EmuTypeGetTime, req, res);

   sift_assert(emulated);
   tv->tv_sec = res.gettime.time_ns / 1000000000;
   tv->tv_usec = (res.gettime.time_ns / 1000) % 1000000;

   return 0;
}

static void rtnCallback(RTN rtn, VOID *v)
{
   std::string rtn_name = RTN_Name(rtn);

   if (rtn_name == "sched_getcpu")
      RTN_ReplaceSignature(rtn, AFUNPTR(emuGetCPU), IARG_THREAD_ID, IARG_END);
   else if (rtn_name == "get_nprocs")
      RTN_ReplaceSignature(rtn, AFUNPTR(emuGetNprocs), IARG_THREAD_ID, IARG_END);
   else if (rtn_name == "get_nprocs_conf" || rtn_name == "__get_nprocs_conf")
      RTN_ReplaceSignature(rtn, AFUNPTR(emuGetNprocs), IARG_THREAD_ID, IARG_END);
   else if (rtn_name == "clock_gettime")
      RTN_ReplaceSignature(rtn, AFUNPTR(emuClockGettime), IARG_THREAD_ID,
                           IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_END);
   else if (rtn_name.find("gettimeofday") != std::string::npos)
      RTN_ReplaceSignature(rtn, AFUNPTR(emuGettimeofday), IARG_THREAD_ID,
                           IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_END);
}

void initEmulation()
{
   INS_AddInstrumentFunction(insCallback, 0);
   RTN_AddInstrumentFunction(rtnCallback, 0);
}
