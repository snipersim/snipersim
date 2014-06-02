#include "emulation.h"
#include "recorder_control.h"
#include "sift_assert.h"
#include "globals.h"
#include "threads.h"

#include <pin.H>

static AFUNPTR ptr_exit = NULL;

static void handleRdtsc(THREADID threadid, PIN_REGISTER * gax, PIN_REGISTER * gdx)
{
   if (!thread_data[threadid].output)
      return;

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

static void handleCpuid(THREADID threadid, PIN_REGISTER * gax, PIN_REGISTER * gbx, PIN_REGISTER * gcx, PIN_REGISTER * gdx)
{
   if (!thread_data[threadid].output)
      return;

   Sift::EmuRequest req;
   Sift::EmuReply res;

   req.cpuid.eax = gax->dword[0];
   req.cpuid.ecx = gcx->dword[0];
   bool emulated = thread_data[threadid].output->Emulate(Sift::EmuTypeCpuid, req, res);

   sift_assert(emulated);
   gax->dword[0] = res.cpuid.eax;
   gbx->dword[0] = res.cpuid.ebx;
   gcx->dword[0] = res.cpuid.ecx;
   gdx->dword[0] = res.cpuid.edx;
}

static ADDRINT emuGetNprocs(THREADID threadid)
{
   if (!thread_data[threadid].output)
      return 1;

   Sift::EmuRequest req;
   Sift::EmuReply res;
   bool emulated = thread_data[threadid].output->Emulate(Sift::EmuTypeGetProcInfo, req, res);

   sift_assert(emulated);
   return res.getprocinfo.emunprocs;
}

static ADDRINT emuGetCPU(THREADID threadid)
{
   if (!thread_data[threadid].output)
      return 0;

   Sift::EmuRequest req;
   Sift::EmuReply res;
   bool emulated = thread_data[threadid].output->Emulate(Sift::EmuTypeGetProcInfo, req, res);

   sift_assert(emulated);
   return res.getprocinfo.procid;
}

ADDRINT emuClockGettime(THREADID threadid, clockid_t clk_id, struct timespec *tp)
{
   if (!thread_data[threadid].output)
      return 0;

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
   if (!thread_data[threadid].output)
      return 0;

   //sift_assert(tz == NULL); // gettimeofday() with non-NULL timezone not supported
   sift_assert(tv != NULL); // gettimeofday() called with NULL timeval not supported

   Sift::EmuRequest req;
   Sift::EmuReply res;
   bool emulated = thread_data[threadid].output->Emulate(Sift::EmuTypeGetTime, req, res);
   sift_assert(emulated);

   // Make sure time seems to always increase (even in fast-forward mode when there is no timing model)
   static uint64_t t_last = 0;
   if (t_last >= res.gettime.time_ns)
      res.gettime.time_ns = t_last + 1000;
   t_last = res.gettime.time_ns;

   tv->tv_sec = res.gettime.time_ns / 1000000000;
   tv->tv_usec = (res.gettime.time_ns / 1000) % 1000000;

   return 0;
}

void emuKmpReapMonitor(THREADID threadIndex, CONTEXT *ctxt)
{
   // Hack to make ICC's OpenMP runtime library work.
   // This runtime creates a monitor thread which blocks in a condition variable with a timeout.
   // On exit, thread 0 executes __kmp_reap_monitor() which join()s on this monitor thread.
   // In due time, the timeout occurs and the monitor thread returns
   // from pthread_cond_timedwait(), sees that it should be exiting, and returns.
   // However, in simulation the timeout value may be wrong (if gettimeofday isn't properly replaced)
   // so the timout doesn't reliably occur. Instead, call exit() here to
   // forcefully terminate the application when the master thread reaches __kmp_reap_monitor().
   for (unsigned int i = 0 ; i < MAX_NUM_THREADS ; i++)
   {
      if (thread_data[i].output)
      {
         closeFile(i);
      }
   }

   void *res;
   PIN_CallApplicationFunction(ctxt, threadIndex, CALLINGSTD_DEFAULT, ptr_exit, PIN_PARG(void*), &res, PIN_PARG(int), 0, PIN_PARG_END());
}

static void insCallback(INS ins, VOID *v)
{
   if (INS_IsRDTSC(ins))
      INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR)handleRdtsc, IARG_THREAD_ID, IARG_REG_REFERENCE, REG_GAX, IARG_REG_REFERENCE, REG_GDX, IARG_END);

   if (INS_Opcode(ins) == XED_ICLASS_CPUID)
   {
      INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)handleCpuid, IARG_THREAD_ID, IARG_REG_REFERENCE, REG_GAX, IARG_REG_REFERENCE, REG_GBX, IARG_REG_REFERENCE, REG_GCX, IARG_REG_REFERENCE, REG_GDX, IARG_END);
      INS_Delete(ins);
   }
}

static void traceCallback(TRACE trace, VOID *v)
{
   RTN rtn = TRACE_Rtn(trace);
   if (RTN_Valid(rtn) && RTN_Address(rtn) == TRACE_Address(trace))
   {
      std::string rtn_name = RTN_Name(rtn);

      // icc/openmp compatibility
      if (rtn_name == "__kmp_reap_monitor" || rtn_name == "__kmp_internal_end_atexit")
      {
         TRACE_InsertCall(trace, IPOINT_BEFORE, AFUNPTR(emuKmpReapMonitor), IARG_THREAD_ID, IARG_CONTEXT, IARG_END);
      }
   }
}

static void rtnCallback(RTN rtn, VOID *v)
{
   std::string rtn_name = RTN_Name(rtn);

   if (rtn_name == "sched_getcpu")
      RTN_ReplaceSignature(rtn, AFUNPTR(emuGetCPU), IARG_THREAD_ID, IARG_END);
   else if (rtn_name == "get_nprocs" || rtn_name == "__get_nprocs")
      RTN_ReplaceSignature(rtn, AFUNPTR(emuGetNprocs), IARG_THREAD_ID, IARG_END);
   else if (rtn_name == "get_nprocs_conf" || rtn_name == "__get_nprocs_conf")
      RTN_ReplaceSignature(rtn, AFUNPTR(emuGetNprocs), IARG_THREAD_ID, IARG_END);
   else if (rtn_name == "clock_gettime")
      RTN_ReplaceSignature(rtn, AFUNPTR(emuClockGettime), IARG_THREAD_ID,
                           IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_END);
   else if (rtn_name.find("gettimeofday") != std::string::npos)
      RTN_ReplaceSignature(rtn, AFUNPTR(emuGettimeofday), IARG_THREAD_ID,
                           IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_END);

   // save pointers to functions we'll want to call through PIN_CallApplicationFunction
   if (rtn_name == "exit")                   ptr_exit = RTN_Funptr(rtn);
}

void initEmulation()
{
   if (KnobEmulateSyscalls.Value())
   {
      INS_AddInstrumentFunction(insCallback, 0);
      TRACE_AddInstrumentFunction(traceCallback, 0);
      RTN_AddInstrumentFunction(rtnCallback, 0);
   }
}
