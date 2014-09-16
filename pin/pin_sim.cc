// Harshad Kasture, Jason Miller, Chris Celio, Charles Gruenwald,
// Nathan Beckmann, George Kurian, David Wentzlaff, James Psota
// 10.12.08
//
// Carbon Computer Simulator
//
// This simulator models future multi-core computers with thousands of cores.
// It runs on today's x86 multicores and will scale as more and more cores
// and better inter-core communications mechanisms become available.
// The simulator provides a platform for research in processor architecture,
// compilers, network interconnect topologies, and some OS.
//
// The simulator runs on top of Intel's Pin dynamic binary instrumention engine.
// Application code in the absence of instrumentation runs more or less
// natively and is thus high performance. When instrumentation is used, models
// can be hot-swapped or dynamically enabled and disabled as desired so that
// performance tracks the level of simulation detail needed.


// FIXME: This list could probably be trimmed down a lot.
#include "simulator.h"
#include "core_manager.h"
#include "config.h"
#include "core.h"
#include "thread.h"
#include "syscall_model.h"
#include "thread_manager.h"
#include "hooks_manager.h"
#include "trace_manager.h"
#include "config_file.hpp"
#include "handle_args.h"
#include "log.h"
#include "instruction_modeling.h"
#include "instruction.h"
#include "magic_client.h"
#include "sampling_manager.h"
#include "sampling_provider.h"
#include "performance_model.h"
#include "timer.h"
#include "logmem.h"
#include "circular_log.h"

#include "codecache_trace.h"
#include "local_storage.h"
#include "toolreg.h"
#include "pin_exceptions.h"
#include "trace_rtn.h"

// lite directories
#include "lite/routine_replace.h"
#include "lite/handle_syscalls.h"

#include "sim_api.h"

#include "pin.H"
#include "inst_mode_macros.h"
#ifdef PINPLAY_SUPPORTED
# include "pinplay.H"
#endif

#include <iostream>
#include <assert.h>
#include <set>
#include <sys/syscall.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

#include <typeinfo>

// ---------------------------------------------------------------
// FIXME:
// There should be a better place to keep these globals
// -- a PinSimulator class or smthg
bool done_app_initialization = false;
config::ConfigFile *cfg;

// clone stuff
extern int *parent_tidptr;
#ifdef TARGET_IA32
extern struct user_desc *newtls;
#endif
extern int *child_tidptr;

bool forkedInChild = false;

// ---------------------------------------------------------------

map <ADDRINT, string> rtn_map;
PIN_LOCK rtn_map_lock;

#ifdef PINPLAY_SUPPORTED
PINPLAY_ENGINE pinplay_engine;
#endif

void applicationMemCopy(void *dest, const void *src, size_t n)
{
   PIN_SafeCopy(dest, src, n);
}

void printRtn (ADDRINT rtn_addr, bool enter)
{
   PIN_GetLock (&rtn_map_lock, 1);
   map<ADDRINT, string>::iterator it = rtn_map.find (rtn_addr);

   string point = enter ? "Enter" : "Exit";
   if (it != rtn_map.end())
   {
      LOG_PRINT ("Stack trace : %s %s", point.c_str(), (it->second).c_str());
   }
   else
   {
      LOG_PRINT ("Stack trace : %s UNKNOWN", point.c_str());
   }

   PIN_ReleaseLock (&rtn_map_lock);
}

VOID printInsInfo(CONTEXT* ctxt)
{
   __attribute__((__unused__)) ADDRINT reg_inst_ptr = PIN_GetContextReg(ctxt, REG_INST_PTR);
   __attribute__((__unused__)) ADDRINT reg_stack_ptr = PIN_GetContextReg(ctxt, REG_STACK_PTR);

   LOG_PRINT("eip = %#llx, esp = %#llx", reg_inst_ptr, reg_stack_ptr);
}

void showInstructionInfo(INS ins)
{
   printf("\t");
   if (INS_IsMemoryRead(ins) || INS_IsMemoryWrite(ins))
      printf("* ");
   else
      printf("  ");
//   printf("%d - %s ", INS_Category(ins), CATEGORY_StringShort(INS_Category(ins)).c_str());
   printf("%x - %s ", INS_Opcode(ins), OPCODE_StringShort(INS_Opcode(ins)).c_str());
   printf(" %s ", INS_Disassemble(ins).c_str());
   printf("\n");
}

VOID instructionCallback(TRACE trace, INS ins, InstMode::inst_mode_t inst_mode)
{
   // Debugging Functions
   // showInstructionInfo(ins);
   if (Log::getSingleton()->isLoggingEnabled())
   {
      INS_InsertPredicatedCall(ins, IPOINT_BEFORE,
            AFUNPTR(printInsInfo),
            IARG_CALL_ORDER, CALL_ORDER_FIRST,
            IARG_CONTEXT,
            IARG_END);
   }

   // Core Performance Modeling

}

void handleCheckScheduled(THREADID threadIndex)
{
   Thread* thread = localStore[threadIndex].thread;
   Core* core = thread->getCore();

   if (core == NULL)
   {
      // This thread isn't scheduled. Normally it shouldn't execute any code,
      // but just after thread start we do get here.
      // Wait here until we're scheduled on one of the cores.
      SubsecondTime time = SubsecondTime::Zero();
      thread->reschedule(time, core);

      // We should be on a core now, set its performance model to our local time
      core = thread->getCore();
      // If the core already has a later time, we have to wait
      time = std::max(time, core->getPerformanceModel()->getElapsedTime());
      core->getPerformanceModel()->queuePseudoInstruction(new SpawnInstruction(time));
   }
}

VOID addCheckScheduled(TRACE trace, INS ins_head)
{
   INS_InsertCall(ins_head, IPOINT_BEFORE, AFUNPTR(handleCheckScheduled), IARG_THREAD_ID, IARG_END);
}

static int getInstMode()
{
   return Sim()->getInstrumentationMode();
}

VOID traceCallback(TRACE trace, void *v)
{
   InstMode::inst_mode_t inst_mode = INSTR_GET_MODE(trace);

   BBL bbl_head = TRACE_BblHead(trace);
   INS ins_head = BBL_InsHead(bbl_head);

   #ifdef PINPLAY_SUPPORTED
   // Make sure all version switches happen before any PinPlay instrumentation, to avoid PinPlay seeing some instructions twice
   CALL_ORDER call_order = (CALL_ORDER)(pinplay_engine.PinPlayFirstBeforeCallOrder() - 1);
   #else
   CALL_ORDER call_order = CALL_ORDER_DEFAULT;
   #endif

   // Write the resulting mode to REG_INST_Gx for use by INS_InsertVersionCase
   INS_InsertCall(ins_head, IPOINT_BEFORE, (AFUNPTR)getInstMode, IARG_RETURN_REGS, g_toolregs[TOOLREG_TEMP], IARG_CALL_ORDER, call_order, IARG_END);

   // Add version switch cases for all possible target versions (no test to switch to self)
   #define SWITCH_VERSION(v) if (inst_mode != (v)) INS_InsertVersionCase(ins_head, g_toolregs[TOOLREG_TEMP], v, v, IARG_CALL_ORDER, call_order, IARG_END);
   SWITCH_VERSION(InstMode::DETAILED)
   SWITCH_VERSION(InstMode::CACHE_ONLY)
   SWITCH_VERSION(InstMode::FAST_FORWARD)

   // Version 0 is only for startup / amnesia, don't do anything else there
   if (TRACE_Version(trace) == 0)
      return;

   addCheckScheduled(trace, ins_head);

   addRtnTracer(trace);

   // Routine replacement
   RTN rtn = TRACE_Rtn(trace);
   if (RTN_Valid(rtn)
      && RTN_Address(rtn) == TRACE_Address(trace))
   {
      lite::routineStartCallback(rtn, ins_head);
   }

   // Select per-basicblock instrumentation level.
   // In fast-forward mode, this is the only instrumentation (except for some rare stuff like magic, syscall),
   //   so using a lighter version here significantly speeds up simulation

   InstrumentLevel::Level instr_level;
   if (Sim()->getConfig()->getBBVsEnabled()) {
      // Someone (a Python script) has requested BBVs to be enabled.
      instr_level = InstrumentLevel::INSTR_WITH_BBVS;
   } else if (Sim()->getSamplingManager()->getSamplingProvider()) {
      // Ask the sampling provider what mode they need
      instr_level = Sim()->getSamplingManager()->getSamplingProvider()->requestedInstrumentation();
   } else {
      instr_level = InstrumentLevel::INSTR;
   }

   for (BBL bbl = bbl_head; BBL_Valid(bbl); bbl = BBL_Next(bbl))
   {
      switch (instr_level) {
         case (InstrumentLevel::INSTR):
         case (InstrumentLevel::INSTR_WITH_BBVS):
            BBL_InsertCall(bbl, IPOINT_ANYWHERE, (AFUNPTR)InstructionModeling::countInstructions, IARG_THREAD_ID, IARG_ADDRINT, BBL_Address(bbl), IARG_UINT32, BBL_NumIns(bbl), IARG_END);
            break;
         case (InstrumentLevel::NONE):
            break;
         default:
            LOG_PRINT_ERROR("Unexpected enum value '%d'", instr_level);
            break;
      }

      // I-cache modeling during warmup
      if (Sim()->getConfig()->getEnableICacheModeling()) {
         INSTRUMENT(
            INSTR_IF_CACHEONLY(inst_mode),
            trace, BBL_InsHead(bbl), IPOINT_BEFORE,
            AFUNPTR(InstructionModeling::accessInstructionCacheWarmup),
            IARG_THREAD_ID,
            IARG_ADDRINT, BBL_Address(bbl),
            IARG_UINT32, BBL_Size(bbl),
            IARG_END);
      }

      UINT32 icount = 0;

      for(INS ins = BBL_InsHead(bbl); ; ins = INS_Next(ins))
      {
         // Instruction modelling
         InstructionModeling::addInstructionModeling(trace, ins, inst_mode);
         ++icount;

         if (ins == BBL_InsTail(bbl) || icount >= 32 || INS_HasRealRep(ins))
         {
            if (INS_HasFallThrough(ins))
               INSTRUMENT(INSTR_IF_DETAILED(inst_mode), trace, ins, IPOINT_AFTER, AFUNPTR(InstructionModeling::handleBasicBlock), IARG_THREAD_ID, IARG_END);
            if (INS_IsBranch(ins))
               INSTRUMENT(INSTR_IF_DETAILED(inst_mode), trace, ins, IPOINT_TAKEN_BRANCH, AFUNPTR(InstructionModeling::handleBasicBlock), IARG_THREAD_ID, IARG_END);
            icount = 0;
         }

         if (ins == BBL_InsTail(bbl))
            break;
      }
   }
}

VOID traceInvalidate(ADDRINT orig_pc, ADDRINT cache_pc, BOOL success)
{
   if (!Sim()->getConfig()->getEnableSMCSupport())
   {
      // It should be safe to turn on SMC support at runtime
      // (which means: stop using caches for basic blocks and decoded instruction from now on).
      // Just in case enabling this half-way through isn't reliable, keep the warning for now.
      LOG_PRINT_WARNING_ONCE("Trace invalidation orig_pc(%p) cache_pc(%p) success(%d)\n\n"
                             "Self-modifying code (SMC) support now enabled.\n"
                             "Use general/enable_smc_support=true to enable manually.\n", orig_pc, cache_pc, success);
      Sim()->getConfig()->forceEnableSMCSupport();
   }
}

void ApplicationStart()
{
}

void ApplicationExit(int, void*)
{
   if (forkedInChild)
      return;

   LOG_PRINT("Application exit.");
   Simulator::release();

   delete cfg;
}

void ApplicationDetach(void*)
{
   ApplicationExit(0, NULL);
}

void PinDetach(void)
{
   //PIN_Detach();
}

VOID threadStartCallback(THREADID threadIndex, CONTEXT *ctxt, INT32 flags, VOID *v)
{
   if (! done_app_initialization)
   {
      // Spawn the main() thread
      Sim()->getThreadManager()->spawnThread(INVALID_THREAD_ID, 0);

      // Start the trace manager, if any
      if (Sim()->getTraceManager())
         Sim()->getTraceManager()->start();

      // All the real initialization is done in
      // replacement_start at the moment
      done_app_initialization = true;
   }

   SubsecondTime time;
   thread_id_t thread_id = Sim()->getThreadManager()->getThreadToSpawn(time);

   // Set thread name for Sniper-in-Sniper simulations
   String threadName = String("app-") + itostr(thread_id);
   SimSetThreadName(threadName.c_str());

   Sim()->getThreadManager()->onThreadStart(thread_id, time);

   // Don't resize this vector at runtime as it's shared and we don't want it to be reallocated while someone uses it
   LOG_ASSERT_ERROR(localStore.size() > threadIndex, "Ran out of thread_id slots, increase MAX_PIN_THREADS");

   memset(&localStore[threadIndex], 0, sizeof(localStore[threadIndex]));
   localStore[threadIndex].thread = Sim()->getThreadManager()->getThreadFromID(thread_id);
   localStore[threadIndex].thread->m_os_info.tid = syscall(__NR_gettid);
   if (Sim()->getConfig()->getEnableSpinLoopDetection())
      localStore[threadIndex].sld.sld = new SpinLoopDetector(localStore[threadIndex].thread);
   for (unsigned int idx = 0; idx < ThreadLocalStorage::NUM_SCRATCHPADS; ++idx)
   {
      int status = posix_memalign((void**) &localStore[threadIndex].scratch[idx], 4 * sizeof (void*), ThreadLocalStorage::SCRATCHPAD_SIZE);
      assert(status == 0);
   }
}

VOID threadFiniCallback(THREADID threadIndex, const CONTEXT *ctxt, INT32 flags, VOID *v)
{
   if (forkedInChild)
      return;

   Sim()->getThreadManager()->onThreadExit(localStore[threadIndex].thread->getId());

   if (localStore[threadIndex].thread->getId() == 0)
      if (Sim()->getTraceManager())
         Sim()->getTraceManager()->stop();
}

bool handleSigUsr1(THREADID threadIndex, INT32 signal, CONTEXT *ctx, BOOL hasHandler, const EXCEPTION_INFO *pExceptInfo, void* v)
{
   Sim()->getHooksManager()->callHooks(HookType::HOOK_SIGUSR1, 0);
   return false;
}

VOID forkAfterChild(THREADID threadid, const CONTEXT *ctxt, VOID *v)
{
   // Application has fork()ed. We don't want to track child processes, but Pin will attempt to do cleanups.
   // Make sure that these don't trigger any simulator code.
   forkedInChild = true;
}

int main(int argc, char *argv[])
{
   // ---------------------------------------------------------------
   // FIXME:
   PIN_InitLock (&rtn_map_lock);
   // ---------------------------------------------------------------

   // Global initialization
   PIN_InitSymbols();
   PIN_Init(argc,argv);

   initToolregs();

   // To make sure output shows up immediately, make stdout and stderr line buffered
   // (if we're writing into a pipe to run-graphite, or redirected to a file by the job runner, the default will be block buffered)
   setvbuf(stdout, NULL, _IOLBF, 0);
   setvbuf(stderr, NULL, _IOLBF, 0);

   const char *ld_orig = getenv("SNIPER_SCRIPT_LD_LIBRARY_PATH");
   if (ld_orig)
      setenv("LD_LIBRARY_PATH", ld_orig, 1);

   string_vec args;

   // Set the default config path if it isn't
   // overwritten on the command line.
   String config_path = "carbon_sim.cfg";

   parse_args(args, config_path, argc, argv);

   cfg = new config::ConfigFile();
   cfg->load(config_path);

   handle_args(args, *cfg);

   Simulator::setConfig(cfg, Config::PINTOOL);

   Simulator::allocate();
   Sim()->start();

   // If -appdebug_enable is used, write out the port to connect GDB to
   if(PIN_GetDebugStatus() != DEBUG_STATUS_DISABLED) {
      DEBUG_CONNECTION_INFO info;
      if (PIN_GetDebugConnectionInfo(&info) && info._type == DEBUG_CONNECTION_TYPE_TCP_SERVER) {
         FILE* fp = fopen(Sim()->getConfig()->formatOutputFileName("appdebug_port.out").c_str(), "w");
         fprintf(fp, "%d %d", PIN_GetPid(), info._tcpServer._tcpPort);
         fclose(fp);
      }
      String scheme = Sim()->getCfg()->getString("clock_skew_minimization/scheme");
      if (!(scheme == "none" || scheme == "barrier")) {
         fprintf(stderr, "\n[WARNING] Application debugging is not compatible with %s synchronization.\n", scheme.c_str());
         fprintf(stderr, "          Consider adding -g --clock_skew_minimization/scheme={none|barrier}\n\n");
      }
   }

   // Instrumentation
   LOG_PRINT("Start of instrumentation.");

   PIN_AddThreadStartFunction (threadStartCallback, 0);
   PIN_AddThreadFiniFunction (threadFiniCallback, 0);

   if (Sim()->getConfig()->getEnableSyscallEmulation())
   {
      PIN_AddSyscallEntryFunction(lite::syscallEnterRunModel, 0);
      PIN_AddSyscallExitFunction(lite::syscallExitRunModel, 0);
   }
   bool enable_signals = cfg->getBool("general/enable_signals");
   if (!enable_signals) {
      PIN_InterceptSignal(SIGILL, lite::interceptSignal, NULL);
      PIN_InterceptSignal(SIGFPE, lite::interceptSignal, NULL);
      PIN_InterceptSignal(SIGSEGV, lite::interceptSignal, NULL);
   }
   PIN_InterceptSignal(SIGUSR1, handleSigUsr1, NULL);
   PIN_AddInternalExceptionHandler(exceptionHandler, NULL);

   RTN_AddInstrumentFunction(lite::routineCallback, 0);

   TRACE_AddInstrumentFunction(traceCallback, 0);

   if (!Sim()->getConfig()->getEnableSMCSupport())
      CODECACHE_AddTraceInvalidatedFunction(traceInvalidate, 0);

   PIN_AddDetachFunction(ApplicationDetach, 0);
   PIN_AddFiniUnlockedFunction(ApplicationExit, 0);

   PIN_AddForkFunction(FPOINT_AFTER_IN_CHILD, forkAfterChild, 0);

   if (cfg->getBool("log/pin_codecache_trace"))
      initCodeCacheTracing();

   String syntax = cfg->getString("general/syntax");
   if (syntax == "intel")
      PIN_SetSyntaxIntel();
   else if (syntax == "att")
      PIN_SetSyntaxATT();
   else if (syntax == "xed")
      PIN_SetSyntaxXED();
   else
      LOG_PRINT_ERROR("Unknown assembly syntax %s, should be intel, att or xed.", syntax.c_str());

   // Just in case ... might not be strictly necessary
// PIN_SpawnInternalThread doesn't schedule its threads until after PIN_StartProgram
//   Transport::getSingleton()->barrier();

#ifdef PINPLAY_SUPPORTED
   if (Sim()->getConfig()->getEnablePinPlay())
   {
      pinplay_engine.Activate(argc, argv, false /*logger*/, true /*replayer*/);
      LOG_ASSERT_ERROR(Sim()->getConfig()->getEnableSyscallEmulation() == false,
                       "System call emulation must be disabled when using PinPlay.");
   }
#else
   LOG_ASSERT_ERROR(Sim()->getConfig()->getEnablePinPlay() == false,
                    "PinPlay support not compiled in. Please use a compatible pin kit when compiling.");
#endif

   // config::Config shouldn't be called outside of init/fini
   // With Sim()->hideCfg(), we let Simulator know to complain when someone does call Sim()->getCfg()
   Sim()->hideCfg();

   // Never returns
   LOG_PRINT("Running program...");
   PIN_StartProgram();

   return 0;
}
