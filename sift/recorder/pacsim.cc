#include "pacsim.h"
#include "pacsim_sampler.h"
#include "globals.h"
#include "threads.h"
#include "recorder_control.h"

#include "pin.H"
#include <iostream>
#include <string>

namespace pacsim
{

/* What the sampler needs from this recorder. */
class PinFrontEnd : public FrontEnd
{
   PIN_MUTEX m_mutex;
public:
   PinFrontEnd() { PIN_MutexInit(&m_mutex); }
   bool inRoi() { return in_roi; }
   bool channelAlive(unsigned tid) { return thread_data[tid].output != NULL; }
   uint64_t magic(unsigned tid, uint64_t cmd, uint64_t arg) { return thread_data[tid].output->Magic(cmd, arg, 0); }
   void setDetailed() { setInstrumentationMode(Sift::ModeDetailed); }
   void setFastForward(FastForwardKind kind) { setInstrumentationMode(kind == FastForwardKind::Warmup ? Sift::ModeMemory : Sift::ModeIcount); }
   void flushCounts(unsigned tid)
   {
      if (thread_data[tid].output && thread_data[tid].icount_reported > 0)
      {
         thread_data[tid].output->InstructionCount(thread_data[tid].icount_reported);
         thread_data[tid].icount_reported = 0;
      }
   }
   bool tryLock() { return PIN_MutexTryLock(&m_mutex); }
   void lock() { PIN_MutexLock(&m_mutex); }
   void unlock() { PIN_MutexUnlock(&m_mutex); }
   void log(const std::string &line) { std::cerr << line << std::endl; }
};

static PinFrontEnd *g_front_end = NULL;
static Sampler *g_sampler = NULL;

static VOID onBlock(THREADID tid, UINT32 n, ADDRINT addr) { g_sampler->block(tid, n, addr); }
static VOID onBackEdge(THREADID tid, ADDRINT addr) { g_sampler->backEdge(tid, addr); }
static VOID onSync(THREADID tid, ADDRINT addr) { g_sampler->syncPoint(tid, addr); }
static VOID onExit(THREADID tid, ADDRINT addr) { g_sampler->finish(tid, addr, in_roi && thread_data[tid].output); }

static VOID traceCallback(TRACE trace, VOID *)
{
   for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
      BBL_InsertCall(bbl, IPOINT_ANYWHERE, (AFUNPTR)onBlock, IARG_THREAD_ID, IARG_UINT32, BBL_NumIns(bbl), IARG_ADDRINT, BBL_Address(bbl), IARG_END);
   INS tail = BBL_InsTail(TRACE_BblTail(trace));
   if (INS_IsDirectControlFlow(tail) && INS_HasFallThrough(tail) && INS_DirectControlFlowTargetAddress(tail) < INS_Address(tail))
      INS_InsertPredicatedCall(tail, IPOINT_BEFORE, (AFUNPTR)onBackEdge, IARG_THREAD_ID, IARG_INST_PTR, IARG_END);
   RTN rtn = TRACE_Rtn(trace);
   if (RTN_Valid(rtn) && RTN_Address(rtn) == TRACE_Address(trace))
   {
      const std::string name = RTN_Name(rtn);
      if (name.find("_omp_fn") != std::string::npos)          /* a GCC OpenMP outlined region */
         INS_InsertPredicatedCall(BBL_InsHead(TRACE_BblHead(trace)), IPOINT_BEFORE, (AFUNPTR)onSync, IARG_THREAD_ID, IARG_INST_PTR, IARG_END);
   }
}

static VOID routineCallback(RTN rtn, VOID *)
{
   const std::string name = RTN_Name(rtn);
   if (name == "_exit" || name == "_Exit")
   {
      RTN_Open(rtn);
      RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)onExit, IARG_THREAD_ID, IARG_INST_PTR, IARG_END);
      RTN_Close(rtn);
   }
   else if (name == "pthread_barrier_wait" || name == "GOMP_barrier" || name == "GOMP_parallel" || name == "__kmp_barrier"
            || name == "__kmpc_barrier" || name == "__kmp_invoke_microtask" || name == "MPI_Barrier" || name == "PMPI_Barrier")
   {
      /* a synchronisation point, keyed by its call site (the barrier
         routine itself is the same address for every barrier) */
      RTN_Open(rtn);
      RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)onSync, IARG_THREAD_ID, IARG_RETURN_IP, IARG_END);
      RTN_Close(rtn);
   }
}

static VOID threadStart(THREADID tid, CONTEXT *, INT32, VOID *) { g_sampler->threadStart(tid); }
static VOID fini(INT32, VOID *) { g_sampler->finish(0, 0, false); }

void roiBegin(int tid)
{
   if (g_sampler)
      g_sampler->roiBegin(tid);
}

void roiEnd(int tid)
{
   if (g_sampler)
      g_sampler->finish(tid, 0, true);
}

void activate()
{
   Options o;
   o.region_size = KnobPacSimRegionSize.Value() * 1000;
   o.min_region_size = KnobPacSimMinRegionSize.Value() * 1000;
   o.cluster_threshold = KnobPacSimClusterThreshold.Value();
   o.min_measurements = KnobPacSimMinMeasurements.Value();
   o.fast_forward = KnobPacSimWarmup.Value() ? FastForwardKind::Warmup : FastForwardKind::Icount;
   o.max_threads = MAX_NUM_THREADS_DEFAULT;
   o.output_dir = KnobPacSimDir.Value().c_str();
   o.verbose = KnobVerbose.Value();
   g_front_end = new PinFrontEnd();
   g_sampler = new Sampler(o, *g_front_end);
   std::cerr << "[PacSim] online sampling enabled (regions of " << KnobPacSimRegionSize.Value() << "K instructions, cluster threshold "
             << KnobPacSimClusterThreshold.Value() << ", " << (KnobPacSimWarmup.Value() ? "cache warm-up" : "instruction counting") << " between measured regions, output " << KnobPacSimDir.Value() << ")" << std::endl;
   TRACE_AddInstrumentFunction(traceCallback, NULL);
   RTN_AddInstrumentFunction(routineCallback, NULL);
   PIN_AddThreadStartFunction(threadStart, NULL);
   PIN_AddFiniFunction(fini, NULL);
}

}
