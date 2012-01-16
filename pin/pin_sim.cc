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
#include "basic_block.h"
#include "syscall_model.h"
#include "thread_manager.h"
#include "hooks_manager.h"
#include "config_file.hpp"
#include "handle_args.h"
#include "thread_start.h"
#include "pin_config.h"
#include "log.h"
#include "vm_manager.h"
#include "instruction_modeling.h"
#include "progress_trace.h"
#include "clock_skew_minimization.h"
#include "magic_client.h"

#include "routine_replace.h"
#include "redirect_memory.h"
#include "handle_syscalls.h"
#include "opcodes.h"
#include "codecache_trace.h"
#include "local_storage.h"

// lite directories
#include "lite/routine_replace.h"
#include "lite/memory_modeling.h"
#include "lite/handle_syscalls.h"

#include "pin.H"
#include "inst_mode_macros.h"

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

extern PIN_LOCK clone_memory_update_lock;
// ---------------------------------------------------------------

map <ADDRINT, string> rtn_map;
PIN_LOCK rtn_map_lock;

std::vector<ThreadLocalStorage> localStore(MAX_PIN_THREADS);

void printRtn (ADDRINT rtn_addr, bool enter)
{
   GetLock (&rtn_map_lock, 1);
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

   ReleaseLock (&rtn_map_lock);
}

VOID printInsInfo(CONTEXT* ctxt)
{
   __attribute(__unused__) ADDRINT reg_inst_ptr = PIN_GetContextReg(ctxt, REG_INST_PTR);
   __attribute(__unused__) ADDRINT reg_stack_ptr = PIN_GetContextReg(ctxt, REG_STACK_PTR);

   LOG_PRINT("eip = %#llx, esp = %#llx", reg_inst_ptr, reg_stack_ptr);
}

void instrumentRoutine(RTN rtn, INS ins)
{
   // An analysis routine inserted on RTN_InsHead will be called *after*
   // one inserted on BBL_InsHead(TRACE_BblHead()). So, prefer the ins passed in
   // from the TRACE instrumenation code

   string rtn_name = RTN_Name(rtn);

   replaceUserAPIFunction(rtn, rtn_name, ins);

   // ---------------------------------------------------------------

   String module = Log::getSingleton()->getModule(__FILE__);
   if (Log::getSingleton()->isEnabled(module.c_str()) &&
       Sim()->getCfg()->getBool("log/stack_trace",false))
   {
      RTN_Open (rtn);

      ADDRINT rtn_addr = RTN_Address (rtn);

      GetLock (&rtn_map_lock, 1);

      rtn_map.insert (make_pair (rtn_addr, rtn_name));

      ReleaseLock (&rtn_map_lock);

      RTN_InsertCall (rtn, IPOINT_BEFORE,
                      AFUNPTR (printRtn),
                      IARG_ADDRINT, rtn_addr,
                      IARG_BOOL, true,
                      IARG_END);

      RTN_InsertCall (rtn, IPOINT_AFTER,
                      AFUNPTR (printRtn),
                      IARG_ADDRINT, rtn_addr,
                      IARG_BOOL, false,
                      IARG_END);

      RTN_Close (rtn);
   }

   // ---------------------------------------------------------------

   if (rtn_name == "CarbonSpawnThreadSpawner")
   {
      INS_InsertCall (ins, IPOINT_BEFORE,
            AFUNPTR (setupCarbonSpawnThreadSpawnerStack),
            IARG_CONTEXT,
            IARG_END);
   }

   else if (rtn_name == "CarbonThreadSpawner")
   {
      INS_InsertCall (ins, IPOINT_BEFORE,
            AFUNPTR (setupCarbonThreadSpawnerStack),
            IARG_CONTEXT,
            IARG_END);
   }
}

void showInstructionInfo(INS ins)
{
   if (Sim()->getCoreManager()->getCurrentCore()->getId() != 0)
      return;
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

BOOL instructionCallback(TRACE trace, INS ins, BasicBlock *basic_block)
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
   if (Config::getSingleton()->getEnablePerformanceModeling()) {
      bool ret = InstructionModeling::addInstructionModeling(trace, ins, basic_block);
      if (ret == false)
         return false;
   }

   if (Sim()->getConfig()->getSimulationMode() == Config::FULL)
   {
      // Special handling for futex syscall because of internal Pin lock
      if (INS_IsSyscall(ins))
      {
         INS_InsertPredicatedCall(ins, IPOINT_BEFORE,
               AFUNPTR(handleFutexSyscall),
               IARG_CONTEXT,
               IARG_END);
      }
      else
      {
         // Emulate(/Rewrite) String, Stack and Memory Operations
         if (rewriteStringOp (ins));
         else if (rewriteStackOp (ins));
         else rewriteMemOp (ins);
      }
   }
   else // Sim()->getConfig()->getSimulationMode() == Config::LITE
   {
      if (INS_IsSyscall(ins))
      {
         INS_InsertPredicatedCall(ins, IPOINT_BEFORE,
               AFUNPTR(lite::handleFutexSyscall),
               IARG_THREAD_ID,
               IARG_CONTEXT,
               IARG_END);
      }
      else
      {
         // Instrument Memory Operations
         lite::addMemoryModeling(trace, ins);
      }
   }
   return true;
}

namespace std
{
   template <> struct hash<std::pair<ADDRINT, ADDRINT> > {
      size_t operator()(const std::pair<ADDRINT, ADDRINT> & p) const {
         return (p.first << 32) ^ (p.second);
      }
   };
}
// Cache basic blocks across re-instrumentation.
// Since Pin has it's own (non-unique) way of defining basic blocks,
// key on both the start and the end address of the basic block.
std::unordered_map<std::pair<ADDRINT, ADDRINT>, BasicBlock*> basicblock_cache;

static int updateInstMode(THREADID thread_id)
{
   localStore[thread_id].inst_mode = Sim()->getInstrumentationMode();
   return localStore[thread_id].inst_mode;
}

VOID traceCallback(TRACE trace, void *v)
{
   BBL bbl_head = TRACE_BblHead(trace);
   INS ins_head = BBL_InsHead(bbl_head);

   // Maintain a per-thread copy of the instrumentation mode, updated only at trace boundaries, to make sure
   // all callbacks (handleBasicBlock, memory*, ...) act consistently.
   INS_InsertCall(ins_head, IPOINT_BEFORE, (AFUNPTR)updateInstMode, IARG_THREAD_ID, IARG_RETURN_REGS, REG_INST_G4, IARG_END);

   // Progress Trace
   addProgressTrace(ins_head);

   // Clock Skew Minimization
   addPeriodicSync(trace, ins_head);

   // Routine replacement
   if (Sim()->getConfig()->getSimulationMode() == Config::FULL) {
      RTN rtn = TRACE_Rtn(trace);
      if (RTN_Valid(rtn)
         && RTN_Address(rtn) == TRACE_Address(trace))
      {
         instrumentRoutine(rtn, ins_head);
      }
   }

   for (BBL bbl = bbl_head; BBL_Valid(bbl); bbl = BBL_Next(bbl))
   {
      BBL_InsertCall(bbl, IPOINT_ANYWHERE, (AFUNPTR)InstructionModeling::countInstructions, IARG_THREAD_ID, IARG_ADDRINT, BBL_Address(bbl), IARG_UINT32, BBL_NumIns(bbl), IARG_END);

      BasicBlock *basic_block = NULL;
      bool basic_block_is_new = true;

      // Instruction modeling
      #ifdef ENABLE_PER_BASIC_BLOCK
         std::pair<ADDRINT, ADDRINT> key(INS_Address(BBL_InsHead(bbl)), INS_Address(BBL_InsTail(bbl)));
         if (basicblock_cache.count(key) == 0) {
            basic_block = new BasicBlock();
            basic_block_is_new = true;
            basicblock_cache[key] = basic_block;
         } else {
            basic_block = basicblock_cache[key];
            basic_block_is_new = false;
         }
         // Insert the handleBasicBlock call, before possible rewrite* stuff. We'll fill in the basic_block later
         INSTRUMENT(INSTR_IF_DETAILED_OR_FULL, trace, BBL_InsHead(bbl), IPOINT_BEFORE, AFUNPTR(InstructionModeling::handleBasicBlock), IARG_THREAD_ID, IARG_PTR, basic_block, IARG_END);
      #endif

      for(INS ins = BBL_InsHead(bbl); ; ins = INS_Next(ins)) {
         #ifndef ENABLE_PER_BASIC_BLOCK
            std::pair<ADDRINT, ADDRINT> key(INS_Address(ins), INS_Address(ins));
            if (basicblock_cache.count(key) == 0) {
               basic_block = new BasicBlock();
               basic_block_is_new = true;
               basicblock_cache[key] = basic_block;
            } else {
               basic_block = basicblock_cache[key];
               basic_block_is_new = false;
            }
            INSTRUMENT(INSTR_IF_DETAILED_OR_FULL, trace, ins, IPOINT_BEFORE, AFUNPTR(InstructionModeling::handleBasicBlock), IARG_THREAD_ID, IARG_PTR, basic_block, IARG_END);
         #endif

         bool res = instructionCallback(trace, ins, basic_block_is_new ? basic_block : NULL);
         if (res == false)
            return; // MAGIC instruction aborts trace

         if (ins == BBL_InsTail(bbl))
            break;
      }
   }
}

// syscall model wrappers
void initializeSyscallModeling()
{
   InitLock(&clone_memory_update_lock);
}

void ApplicationStart()
{
}

void ApplicationExit(int, void*)
{
   // If we're still in ROI, make sure we exit properly
   disablePerformanceGlobal();

   LOG_PRINT("Application exit.");
   Simulator::release();
   shutdownProgressTrace();
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
   threadStartProgressTrace();

   // Conditions under which we must initialize a core
   // 1) (!done_app_initialization) && (curr_process_num == 0)
   // 2) (done_app_initialization) && (!thread_spawner)

   if (! done_app_initialization)
   {
      UInt32 curr_process_num = Sim()->getConfig()->getCurrentProcessNum();

      if (Sim()->getConfig()->getSimulationMode() == Config::LITE)
      {
         LOG_ASSERT_ERROR(curr_process_num == 0, "Lite mode can only be run with 1 process");
         Sim()->getCoreManager()->initializeThread(0);
      }
      else // Sim()->getConfig()->getSimulationMode() == Config::FULL
      {
         ADDRINT reg_esp = PIN_GetContextReg(ctxt, REG_STACK_PTR);
         allocateStackSpace();

         if (curr_process_num == 0)
         {
            Sim()->getCoreManager()->initializeThread(0);

            ADDRINT reg_eip = PIN_GetContextReg(ctxt, REG_INST_PTR);
            // 1) Copying over Static Data
            // Get the image first
            PIN_LockClient();
            IMG img = IMG_FindByAddress(reg_eip);
            PIN_UnlockClient();

            LOG_PRINT("Process: 0, Start Copying Static Data");
            copyStaticData(img);
            LOG_PRINT("Process: 0, Finished Copying Static Data");

            // 2) Copying over initial stack data
            LOG_PRINT("Process: 0, Start Copying Initial Stack Data");
            copyInitialStackData(reg_esp, 0);
            LOG_PRINT("Process: 0, Finished Copying Initial Stack Data");
         }
         else
         {
            core_id_t core_id = Sim()->getConfig()->getCurrentThreadSpawnerCoreNum();
            Sim()->getCoreManager()->initializeThread(core_id);

            Core *core = Sim()->getCoreManager()->getCurrentCore();

            // main thread clock is not affected by start-up time of other processes
            core->getNetwork()->netRecv (0, SYSTEM_INITIALIZATION_NOTIFY);

            LOG_PRINT("Process: %i, Start Copying Initial Stack Data");
            copyInitialStackData(reg_esp, core_id);
            LOG_PRINT("Process: %i, Finished Copying Initial Stack Data");
         }

         // Set the current ESP accordingly
         PIN_SetContextReg(ctxt, REG_STACK_PTR, reg_esp);
      }

      // All the real initialization is done in
      // replacement_start at the moment
      done_app_initialization = true;
   }
   else
   {
      // This is NOT the main thread
      // 'application' thread or 'thread spawner'

      if (Sim()->getConfig()->getSimulationMode() == Config::LITE)
      {
         ThreadSpawnRequest req;
         Sim()->getThreadManager()->getThreadToSpawn(&req);
         Sim()->getThreadManager()->dequeueThreadSpawnReq(&req);

         LOG_ASSERT_ERROR(req.core_id < SInt32(Config::getSingleton()->getApplicationCores()),
               "req.core_id(%i), num application cores(%u)", req.core_id, Config::getSingleton()->getApplicationCores());
         Sim()->getThreadManager()->onThreadStart(&req);
      }
      else // Sim()->getConfig()->getSimulationMode() == Config::FULL
      {
         ADDRINT reg_esp = PIN_GetContextReg(ctxt, REG_STACK_PTR);
         core_id_t core_id = PinConfig::getSingleton()->getCoreIDFromStackPtr(reg_esp);

         LOG_ASSERT_ERROR(core_id != -1, "All application threads and thread spawner are cores now");

         if (core_id == Sim()->getConfig()->getCurrentThreadSpawnerCoreNum())
         {
            // 'Thread Spawner' thread
            Sim()->getCoreManager()->initializeThread(core_id);
         }
         else
         {
            // 'Application' thread
            ThreadSpawnRequest* req = Sim()->getThreadManager()->getThreadSpawnReq();

            LOG_ASSERT_ERROR (req != NULL, "ThreadSpawnRequest is NULL !!");

            // This is an application thread
            LOG_ASSERT_ERROR(core_id == req->core_id, "Got 2 different core_ids: req->core_id = %i, core_id = %i", req->core_id, core_id);

            Sim()->getThreadManager()->onThreadStart(req);
         }

#ifdef TARGET_IA32
         // Restore the clone syscall arguments
         PIN_SetContextReg (ctxt, REG_GDX, (ADDRINT) parent_tidptr);
         PIN_SetContextReg (ctxt, REG_GSI, (ADDRINT) newtls);
         PIN_SetContextReg (ctxt, REG_GDI, (ADDRINT) child_tidptr);
#endif

#ifdef TARGET_X86_64
         // Restore the clone syscall arguments
         PIN_SetContextReg (ctxt, REG_GDX, (ADDRINT) parent_tidptr);
         PIN_SetContextReg (ctxt, LEVEL_BASE::REG_R10, (ADDRINT) child_tidptr);
#endif

         __attribute(__unused__) Core *core = Sim()->getCoreManager()->getCurrentCore();
         LOG_ASSERT_ERROR(core, "core(NULL)");

         // Copy over thread stack data
         // copySpawnedThreadStackData(reg_esp);

         // Wait to make sure that the spawner has written stuff back to memory
         // FIXME: What is this for(?) This seems arbitrary
         GetLock (&clone_memory_update_lock, 2);
         ReleaseLock (&clone_memory_update_lock);
      }
   }

   // Don't resize this vector at runtime as it's shared and we don't want it to be reallocated while someone uses it
   LOG_ASSERT_ERROR(localStore.size() > threadIndex, "Ran out of thread_id slots, increase MAX_PIN_THREADS");

   memset(&localStore[threadIndex], 0, sizeof(localStore[threadIndex]));
   localStore[threadIndex].core = Sim()->getCoreManager()->getCurrentCore();
   localStore[threadIndex].inst_mode = Sim()->getInstrumentationMode();
}

VOID threadFiniCallback(THREADID threadIndex, const CONTEXT *ctxt, INT32 flags, VOID *v)
{
   Sim()->getThreadManager()->onThreadExit();
}

int main(int argc, char *argv[])
{
   // ---------------------------------------------------------------
   // FIXME:
   InitLock (&rtn_map_lock);
   // ---------------------------------------------------------------

   // Global initialization
   PIN_InitSymbols();
   PIN_Init(argc,argv);

   // To make sure output shows up immediately, make stdout and stderr line buffered
   // (if we're writing into a pipe to run-graphite, or redirected to a file by the job runner, the default will be block buffered)
   setvbuf(stdout, NULL, _IOLBF, 0);
   setvbuf(stderr, NULL, _IOLBF, 0);

   string_vec args;

   // Set the default config path if it isn't
   // overwritten on the command line.
   String config_path = "carbon_sim.cfg";

   parse_args(args, config_path, argc, argv);

   cfg = new config::ConfigFile();
   cfg->load(config_path);

   handle_args(args, *cfg);

   Simulator::setConfig(cfg);

   Simulator::allocate();
   Sim()->start();

   if (Sim()->getConfig()->getSimulationMode() == Config::FULL)
      PinConfig::allocate();

   VMManager::allocate();

   // Write out the current address of rdtsc(), so tools/addr2line.py can compute the mapping offset
   FILE* fp = fopen(Sim()->getConfig()->formatOutputFileName("debug_offset.out").c_str(), "w");
   fprintf(fp, "%lu\n", (unsigned long)rdtsc);
   fclose(fp);

   // If -appdebug_enable is used, write out the port to connect GDB to
   if(PIN_GetDebugStatus() != DEBUG_STATUS_DISABLED) {
      DEBUG_CONNECTION_INFO info;
      if (PIN_GetDebugConnectionInfo(&info) && info._type == DEBUG_CONNECTION_TYPE_TCP_SERVER) {
         FILE* fp = fopen(Sim()->getConfig()->formatOutputFileName("appdebug_port.out").c_str(), "w");
         fprintf(fp, "%d %d", PIN_GetPid(), info._tcpServer._tcpPort);
         fclose(fp);
      }
      String scheme = Sim()->getCfg()->getString("clock_skew_minimization/scheme","none");
      if (!(scheme == "none" || scheme == "barrier")) {
         fprintf(stderr, "\n[WARNING] Application debugging is not compatible with %s synchronization.\n", scheme.c_str());
         fprintf(stderr, "          Consider adding -g --clock_skew_minimization/scheme={none|barrier}\n\n");
      }
   }

   // Instrumentation
   LOG_PRINT("Start of instrumentation.");

   PIN_AddThreadStartFunction (threadStartCallback, 0);
   PIN_AddThreadFiniFunction (threadFiniCallback, 0);

   if (cfg->getBool("general/enable_syscall_modeling"))
   {
      if (Sim()->getConfig()->getSimulationMode() == Config::FULL)
      {
         initializeSyscallModeling();
         PIN_AddSyscallEntryFunction(syscallEnterRunModel, 0);
         PIN_AddSyscallExitFunction(syscallExitRunModel, 0);
         PIN_AddContextChangeFunction(contextChange, NULL);
      }
      else // Sim()->getConfig()->getSimulationMode() == Config::LITE
      {
         PIN_AddSyscallEntryFunction(lite::syscallEnterRunModel, 0);
         PIN_AddSyscallExitFunction(lite::syscallExitRunModel, 0);
         PIN_InterceptSignal(SIGILL, lite::interceptSignal, NULL);
         PIN_InterceptSignal(SIGFPE, lite::interceptSignal, NULL);
         PIN_InterceptSignal(SIGSEGV, lite::interceptSignal, NULL);
      }
   }

   if (Sim()->getConfig()->getSimulationMode() == Config::LITE)
      RTN_AddInstrumentFunction(lite::routineCallback, 0);

   TRACE_AddInstrumentFunction(traceCallback, 0);

   initProgressTrace();

   PIN_AddDetachFunction(ApplicationDetach, 0);
   PIN_AddFiniUnlockedFunction(ApplicationExit, 0);

   if (cfg->getBool("log/pin_codecache_trace", false))
      initCodeCacheTracing();

   // Just in case ... might not be strictly necessary
// PIN_SpawnInternalThread doesn't schedule its threads until after PIN_StartProgram
//   Transport::getSingleton()->barrier();

   // config::Config shouldn't be called outside of init/fini
   // With Sim()->hideCfg(), we let Simulator know to complain when someone does call Sim()->getCfg()
   Sim()->hideCfg();

   // Never returns
   LOG_PRINT("Running program...");
   PIN_StartProgram();

   return 0;
}
