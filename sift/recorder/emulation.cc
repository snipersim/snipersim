#include "emulation.h"
#include "recorder_control.h"
#include "sift_assert.h"
#include "globals.h"
#include "threads.h"
#include "papi.h"

#include <pin.H>

static AFUNPTR ptr_exit = NULL;

/* The guest's time where the simulator does not answer it (a trace written
   to a file: no response channel): the one-IPC model's, from the same time
   zero as the simulator's (sift/sift_format.h), never the host's.  With
   time_ns the thread's instructions in nanoseconds; the CPU-time clocks
   count from zero. */
static bool live(THREADID threadid)
{
   return thread_data[threadid].output && KnobEmulateSyscalls.Value();
}

static uint64_t one_ipc_ns(THREADID threadid)
{
   return Sift::one_ipc_ns(thread_data[threadid].icount_total);
}

static void handleRdtsc(THREADID threadid, ADDRINT * gax, ADDRINT * gdx)
{
   uint64_t cycles = thread_data[threadid].icount_total;   // the one-IPC model: a cycle an instruction
   Sift::EmuRequest req;
   Sift::EmuReply res;
   if (live(threadid) && thread_data[threadid].output->Emulate(Sift::EmuTypeRdtsc, req, res))
      cycles = res.rdtsc.cycles;
   // Return in eax and edx
   *gdx = cycles >> 32;
   *gax = cycles & 0xffffffff;
}

static void handleCpuid(THREADID threadid, ADDRINT * gax, ADDRINT * gbx, ADDRINT * gcx, ADDRINT * gdx)
{
   if (!thread_data[threadid].output)
      return;

   Sift::EmuRequest req;
   Sift::EmuReply res;

   req.cpuid.eax = *gax;
   req.cpuid.ecx = *gcx;
   bool emulated = thread_data[threadid].output->Emulate(Sift::EmuTypeCpuid, req, res);

   //printf("PIN CPUID: %08lx %08lx => ", *gax, *gcx);
   //printf("%08x %08x %08x %08x\n", res.cpuid.eax, res.cpuid.ebx, res.cpuid.ecx, res.cpuid.edx);

   sift_assert(emulated);
   *gax = res.cpuid.eax;
   *gbx = res.cpuid.ebx;
   *gcx = res.cpuid.ecx;
   *gdx = res.cpuid.edx;
   thread_data[threadid].cpuid_vals = res;
}

static void handleCpuid_assign(THREADID threadid, ADDRINT * gax, ADDRINT * gbx, ADDRINT * gcx, ADDRINT * gdx)
{
   //Those two elements sometimes do not match, even after being assigned by "handleCPUID"
   //printf("After handle CPUID: %08lx %08lx %08lx %08lx\n", *gax, *gbx, *gcx, *gdx);
   //printf("After handle CPUID saved: %08x %08x %08x %08x\n", g_reply.cpuid.eax, g_reply.cpuid.ebx, g_reply.cpuid.ecx, g_reply.cpuid.edx);
   *gax = thread_data[threadid].cpuid_vals.cpuid.eax;
   *gbx = thread_data[threadid].cpuid_vals.cpuid.ebx;
   *gcx = thread_data[threadid].cpuid_vals.cpuid.ecx;
   *gdx = thread_data[threadid].cpuid_vals.cpuid.edx;
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
   /* Every clock is simulated time: the simulator's (GetTime, from its
      osemu/time_start; Counters for the CPU-time clocks, from zero), or the
      one-IPC model's.  The CPU-time clocks -- the process's, the thread's,
      the dynamic ones (negative ids) -- returned failure before. */
   const bool cputime = clk_id == CLOCK_PROCESS_CPUTIME_ID || clk_id == CLOCK_THREAD_CPUTIME_ID || clk_id < 0;
   uint64_t ns = (cputime ? 0 : Sift::TIME_START_DEFAULT * 1000000000ull) + one_ipc_ns(threadid);
   if (live(threadid))
   {
      Sift::EmuRequest req;
      Sift::EmuReply res;
      if (cputime && thread_data[threadid].output->Emulate(Sift::EmuTypeCounters, req, res))
         ns = res.counters.time_ns;
      else if (!cputime && thread_data[threadid].output->Emulate(Sift::EmuTypeGetTime, req, res))
         ns = res.gettime.time_ns;
   }
   if (tp)
   {
      tp->tv_sec = ns / 1000000000;
      tp->tv_nsec = ns % 1000000000;
   }
   return 0;
}

ADDRINT emuGettimeofday(THREADID threadid, struct timeval *tv, struct timezone *tz)
{
   //sift_assert(tz == NULL); // gettimeofday() with non-NULL timezone not supported
   if (tv == NULL)
      return 0;

   uint64_t ns = Sift::TIME_START_DEFAULT * 1000000000ull + one_ipc_ns(threadid);
   Sift::EmuRequest req;
   Sift::EmuReply res;
   if (live(threadid) && thread_data[threadid].output->Emulate(Sift::EmuTypeGetTime, req, res))
      ns = res.gettime.time_ns;

   // Make sure time seems to always increase (even in fast-forward mode when there is no timing model)
   static uint64_t t_last = 0;
   if (t_last >= ns)
      ns = t_last + 1000;
   t_last = ns;

   tv->tv_sec = ns / 1000000000;
   tv->tv_usec = (ns / 1000) % 1000000;

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
   for (unsigned int i = 0 ; i < max_num_threads ; i++)
   {
      if (thread_data[i].output)
      {
         closeFile(i);
      }
   }

   void *res;
   PIN_CallApplicationFunction(ctxt, threadIndex, CALLINGSTD_DEFAULT, ptr_exit, NULL, PIN_PARG(void*), &res, PIN_PARG(int), 0, PIN_PARG_END());
}

static void insCallback(INS ins, VOID *v)
{
   if (INS_IsRDTSC(ins))
      INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR)handleRdtsc, IARG_THREAD_ID, IARG_REG_REFERENCE, REG_GAX, IARG_REG_REFERENCE, REG_GDX, IARG_END);

   if (INS_Opcode(ins) == XED_ICLASS_CPUID)
   {
      INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)handleCpuid, IARG_THREAD_ID, IARG_REG_REFERENCE, REG_GAX, IARG_REG_REFERENCE, REG_GBX, IARG_REG_REFERENCE, REG_GCX, IARG_REG_REFERENCE, REG_GDX, IARG_END);
      INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR)handleCpuid_assign, IARG_THREAD_ID, IARG_REG_REFERENCE, REG_GAX, IARG_REG_REFERENCE, REG_GBX, IARG_REG_REFERENCE, REG_GCX, IARG_REG_REFERENCE, REG_GDX, IARG_END);
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
   else if (rtn_name == "clock_gettime" || rtn_name == "__clock_gettime")
      RTN_ReplaceSignature(rtn, AFUNPTR(emuClockGettime), IARG_THREAD_ID,
                           IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_END);
   else if (rtn_name.find("gettimeofday") != std::string::npos)
      RTN_ReplaceSignature(rtn, AFUNPTR(emuGettimeofday), IARG_THREAD_ID,
                           IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_END);

   // save pointers to functions we'll want to call through PIN_CallApplicationFunction
   if (rtn_name == "exit")                   ptr_exit = RTN_Funptr(rtn);
}

static void timeInsCallback(INS ins, VOID *v)
{
   if (INS_IsRDTSC(ins))
      INS_InsertPredicatedCall(ins, IPOINT_AFTER, (AFUNPTR)handleRdtsc, IARG_THREAD_ID, IARG_REG_REFERENCE, REG_GAX, IARG_REG_REFERENCE, REG_GDX, IARG_END);
}

static void timeRtnCallback(RTN rtn, VOID *v)
{
   std::string rtn_name = RTN_Name(rtn);
   if (rtn_name == "clock_gettime" || rtn_name == "__clock_gettime")
      RTN_ReplaceSignature(rtn, AFUNPTR(emuClockGettime), IARG_THREAD_ID,
                           IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_END);
   else if (rtn_name.find("gettimeofday") != std::string::npos)
      RTN_ReplaceSignature(rtn, AFUNPTR(emuGettimeofday), IARG_THREAD_ID,
                           IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_END);
}

void initEmulation()
{
   if (KnobEmulateSyscalls.Value())
   {
      INS_AddInstrumentFunction(insCallback, 0);
      TRACE_AddInstrumentFunction(traceCallback, 0);
      RTN_AddInstrumentFunction(rtnCallback, 0);
      initPapiInterceptors();
   }
   else
   {
      /* A trace written to a file has no simulator to answer, but the
         guest's time is still not the host's: rdtsc, clock_gettime and
         gettimeofday get the one-IPC model's, from the simulator's time
         zero, so a recording reads the same clock as a live run would
         under the one-IPC model, run after run. */
      INS_AddInstrumentFunction(timeInsCallback, 0);
      RTN_AddInstrumentFunction(timeRtnCallback, 0);
   }
}
