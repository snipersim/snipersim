#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <syscall.h>
#include <vector>
#include <deque>
#include <map>

#include <cstdio>
#include <cassert>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <strings.h>
#include <sys/syscall.h>
#include <sys/file.h>
#include <linux/futex.h>
#include <string.h>
#include <pthread.h>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "pin.H"
#ifdef PINPLAY_SUPPORTED
# include "pinplay.H"
#endif

#include "sift_writer.h"
#include "sift_assert.h"
#include "bbv_count.h"
#include "../../include/sim_api.h"
#include "pinboost_debug.h"

//#define DEBUG_OUTPUT 1
#define DEBUG_OUTPUT 0

#define LINE_SIZE_BYTES 64
#define MAX_NUM_SYSCALLS 4096
#define MAX_NUM_THREADS 128

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool", "o", "trace", "output");
KNOB<UINT64> KnobBlocksize(KNOB_MODE_WRITEONCE, "pintool", "b", "0", "blocksize");
KNOB<UINT64> KnobUseROI(KNOB_MODE_WRITEONCE, "pintool", "roi", "0", "use ROI markers");
KNOB<UINT64> KnobMPIImplicitROI(KNOB_MODE_WRITEONCE, "pintool", "roi-mpi", "0", "Implicit ROI between MPI_Init and MPI_Finalize");
KNOB<UINT64> KnobFastForwardTarget(KNOB_MODE_WRITEONCE, "pintool", "f", "0", "instructions to fast forward");
KNOB<UINT64> KnobDetailedTarget(KNOB_MODE_WRITEONCE, "pintool", "d", "0", "instructions to trace in detail (default = all)");
KNOB<UINT64> KnobUseResponseFiles(KNOB_MODE_WRITEONCE, "pintool", "r", "0", "use response files (required for multithreaded applications or when emulating syscalls, default = 0)");
KNOB<UINT64> KnobEmulateSyscalls(KNOB_MODE_WRITEONCE, "pintool", "e", "0", "emulate syscalls (required for multithreaded applications, default = 0)");
KNOB<BOOL>   KnobSendPhysicalAddresses(KNOB_MODE_WRITEONCE, "pintool", "pa", "0", "send logical to physical address mapping");
KNOB<UINT64> KnobFlowControl(KNOB_MODE_WRITEONCE, "pintool", "flow", "1000", "number of instructions to send before syncing up");
KNOB<INT64> KnobSiftAppId(KNOB_MODE_WRITEONCE, "pintool", "s", "0", "sift app id (default = 0)");
KNOB<BOOL> KnobRoutineTracing(KNOB_MODE_WRITEONCE, "pintool", "rtntrace", "0", "routine tracing");
KNOB<BOOL> KnobDebug(KNOB_MODE_WRITEONCE, "pintool", "debug", "0", "start debugger on internal exception");

# define KNOB_REPLAY_NAME "replay"
# define KNOB_FAMILY "pintool:sift-recorder"
KNOB_COMMENT pinplay_driver_knob_family(KNOB_FAMILY, "PinPlay SIFT Recorder Knobs");
KNOB<BOOL>KnobReplayer(KNOB_MODE_WRITEONCE, KNOB_FAMILY,
                       KNOB_REPLAY_NAME, "0", "Replay a pinball");
#ifdef PINPLAY_SUPPORTED
PINPLAY_ENGINE pinplay_engine;
#endif /* PINPLAY_SUPPORTED */

INT32 app_id;
INT32 num_threads = 0;
UINT64 blocksize;
UINT64 fast_forward_target = 0;
UINT64 detailed_target = 0;
PIN_LOCK access_memory_lock;
PIN_LOCK new_threadid_lock;
std::deque<ADDRINT> tidptrs;
BOOL any_thread_in_detail = false;
const bool verbose = false;
std::unordered_map<ADDRINT, bool> routines;

typedef struct {
   Sift::Writer *output;
   std::deque<ADDRINT> *dyn_address_queue;
   Bbv *bbv;
   UINT64 thread_num;
   ADDRINT bbv_base;
   UINT64 bbv_count;
   ADDRINT bbv_last;
   BOOL bbv_end;
   UINT64 blocknum;
   UINT64 icount;
   UINT64 icount_detailed;
   UINT32 last_syscall_number;
   UINT32 last_syscall_returnval;
   UINT64 flowcontrol_target;
   ADDRINT tid_ptr;
   ADDRINT last_routine;
   BOOL last_syscall_emulated;
   BOOL running;
   #if defined(TARGET_IA32)
      uint8_t __pad[41];
   #elif defined(TARGET_INTEL64)
      uint8_t __pad[13];
   #endif
} __attribute__((packed)) thread_data_t;
thread_data_t *thread_data;

static_assert((sizeof(thread_data_t) % LINE_SIZE_BYTES) == 0, "Error: Thread data should be a multiple of the line size to prevent false sharing");

#if defined(TARGET_IA32)
   typedef uint32_t syscall_args_t[6];
#elif defined(TARGET_INTEL64)
   typedef uint64_t syscall_args_t[6];
#endif

void findMyAppId();
void openFile(THREADID threadid);
void closeFile(THREADID threadid);

static void beginROI(THREADID threadid)
{
   if (app_id < 0)
      findMyAppId();

   if (any_thread_in_detail)
   {
      std::cerr << "[SIFT_RECORDER:" << app_id << "] Error: ROI_START seen, but we have already started." << std::endl;
   }
   else
   {
      if (verbose)
         std::cerr << "[SIFT_RECORDER:" << app_id << "] ROI Begin" << std::endl;
   }
   any_thread_in_detail = true;

   PIN_RemoveInstrumentation();
}

static void endROI(THREADID threadid)
{
   if (KnobEmulateSyscalls.Value())
   {
      // Send SYS_exit_group to the simulator to end the application
      syscall_args_t args = {0};
      args[0] = 0; // Assume success
      thread_data[threadid].output->Syscall(SYS_exit_group, (char*)args, sizeof(args));
   }

   // Delete our .appid file
   char filename[1024] = {0};
   sprintf(filename, "%s.app%" PRId32 ".appid", KnobOutputFile.Value().c_str(), app_id);
   unlink(filename);

   if (verbose)
      std::cerr << "[SIFT_RECORDER:" << app_id << "] ROI End" << std::endl;

   // Stop threads from sending any more data while we close the SIFT pipes
   any_thread_in_detail = false;
   PIN_RemoveInstrumentation();

   for (unsigned int i = 0 ; i < MAX_NUM_THREADS ; i++)
   {
      if (thread_data[i].running && thread_data[i].output)
         closeFile(i);
   }
}

ADDRINT handleMagic(THREADID threadid, ADDRINT gax, ADDRINT gbx, ADDRINT gcx)
{
   if (gax == SIM_CMD_ROI_START)
   {
      if (KnobUseROI.Value() && !any_thread_in_detail)
         beginROI(threadid);
   }
   else if (gax == SIM_CMD_ROI_END)
   {
      if (KnobUseROI.Value() && any_thread_in_detail)
         endROI(threadid);
   }
   else
   {
      if (KnobUseResponseFiles.Value() && thread_data[threadid].running && thread_data[threadid].output)
      {
         uint64_t res = thread_data[threadid].output->Magic(gax, gbx, gcx);
         return res;
      }
   }

   // Default: don't modify gax
   return gax;
}

VOID countInsns(THREADID threadid, INT32 count)
{
   thread_data[threadid].icount += count;

   if (thread_data[threadid].icount >= fast_forward_target && !KnobUseROI.Value() && !KnobMPIImplicitROI.Value())
   {
      if (verbose)
         std::cerr << "[SIFT_RECORDER:" << app_id << ":" << thread_data[threadid].thread_num << "] Changing to detailed after " << thread_data[threadid].icount << " instructions" << std::endl;
      if (!thread_data[threadid].output)
         openFile(threadid);
      thread_data[threadid].icount = 0;
      any_thread_in_detail = true;
      PIN_RemoveInstrumentation();
   }
}

VOID sendInstruction(THREADID threadid, ADDRINT addr, UINT32 size, UINT32 num_addresses, BOOL is_branch, BOOL taken, BOOL is_predicate, BOOL executing, BOOL isbefore, BOOL ispause)
{
   // We're still called for instructions in the same basic block as ROI end, ignore these
   if (!thread_data[threadid].output)
      return;

   ++thread_data[threadid].icount;
   ++thread_data[threadid].icount_detailed;


   // Reconstruct basic blocks (we could ask Pin, but do it the same way as TraceThread will do it)
   if (thread_data[threadid].bbv_end || thread_data[threadid].bbv_last != addr)
   {
      // We're the start of a new basic block
      thread_data[threadid].bbv->count(thread_data[threadid].bbv_base, thread_data[threadid].bbv_count);
      thread_data[threadid].bbv_base = addr;
      thread_data[threadid].bbv_count = 0;
   }
   thread_data[threadid].bbv_count++;
   thread_data[threadid].bbv_last = addr + size;
   // Force BBV end on non-taken branches
   thread_data[threadid].bbv_end = is_branch;


   uint64_t addresses[Sift::MAX_DYNAMIC_ADDRESSES] = { 0 };
   for(uint8_t i = 0; i < num_addresses; ++i)
   {
      addresses[i] = thread_data[threadid].dyn_address_queue->front();
      sift_assert(!thread_data[threadid].dyn_address_queue->empty());
      thread_data[threadid].dyn_address_queue->pop_front();

      if (isbefore)
      {
         // If the instruction hasn't executed yet, access the address to ensure a page fault if the mapping wasn't set up yet
         static char dummy = 0;
         dummy += *(char *)addresses[i];
      }
   }
   sift_assert(thread_data[threadid].dyn_address_queue->empty());

   thread_data[threadid].output->Instruction(addr, size, num_addresses, addresses, is_branch, taken, is_predicate, executing);

   if (KnobUseResponseFiles.Value() && KnobFlowControl.Value() && (thread_data[threadid].icount > thread_data[threadid].flowcontrol_target || ispause))
   {
      thread_data[threadid].output->Sync();
      thread_data[threadid].flowcontrol_target = thread_data[threadid].icount + KnobFlowControl.Value();
   }

   if (detailed_target != 0 && thread_data[threadid].icount_detailed >= detailed_target)
   {
      closeFile(threadid);
      PIN_Detach();
      return;
   }

   if (blocksize && thread_data[threadid].icount >= blocksize)
   {
      openFile(threadid);
      thread_data[threadid].icount = 0;
   }
}

VOID handleMemory(THREADID threadid, ADDRINT address)
{
   // We're still called for instructions in the same basic block as ROI end, ignore these
   if (!thread_data[threadid].output)
      return;

   thread_data[threadid].dyn_address_queue->push_back(address);
}

UINT32 addMemoryModeling(INS ins)
{
   UINT32 num_addresses = 0;

   if (INS_IsMemoryRead (ins) || INS_IsMemoryWrite (ins))
   {
      for (unsigned int i = 0; i < INS_MemoryOperandCount(ins); i++)
      {
         INS_InsertCall(ins, IPOINT_BEFORE,
               AFUNPTR(handleMemory),
               IARG_THREAD_ID,
               IARG_MEMORYOP_EA, i,
               IARG_END);
         num_addresses++;
      }
   }
   sift_assert(num_addresses <= Sift::MAX_DYNAMIC_ADDRESSES);

   return num_addresses;
}

VOID insertCall(INS ins, IPOINT ipoint, UINT32 num_addresses, BOOL is_branch, BOOL taken)
{
   INS_InsertCall(ins, ipoint,
      AFUNPTR(sendInstruction),
      IARG_THREAD_ID,
      IARG_ADDRINT, INS_Address(ins),
      IARG_UINT32, UINT32(INS_Size(ins)),
      IARG_UINT32, num_addresses,
      IARG_BOOL, is_branch,
      IARG_BOOL, taken,
      IARG_BOOL, INS_IsPredicated(ins),
      IARG_EXECUTING,
      IARG_BOOL, ipoint == IPOINT_BEFORE,
      IARG_BOOL, INS_Opcode(ins) == XED_ICLASS_PAUSE,
      IARG_END);
}

// Emulate all system calls
// Do this as a regular callback (versus syscall enter/exit functions) as those hold the global pin lock
VOID emulateSyscallFunc(THREADID threadid, CONTEXT *ctxt)
{
   ADDRINT syscall_number = PIN_GetContextReg(ctxt, REG_GAX);

   sift_assert(syscall_number < MAX_NUM_SYSCALLS);

   syscall_args_t args;
   #if defined(TARGET_IA32)
      args[0] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GBX);
      args[1] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GCX);
      args[2] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GDX);
      args[3] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GSI);
      args[4] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GDI);
      args[5] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GBP);
   #elif defined(TARGET_INTEL64)
      args[0] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GDI);
      args[1] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GSI);
      args[2] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GDX);
      args[3] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_R10);
      args[4] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_R8);
      args[5] = PIN_GetContextReg(ctxt, LEVEL_BASE::REG_R9);
   #else
      #error "Unknown target architecture, require either TARGET_IA32 or TARGET_INTEL64"
   #endif

   // Default: not emulated, override later when needed
   thread_data[threadid].last_syscall_emulated = false;

   if (syscall_number == SYS_write && thread_data[threadid].output)
   {
      int fd = (int)args[0];
      const char *buf = (const char*)args[1];
      size_t count = (size_t)args[2];

      if (count > 0 && (fd == 1 || fd == 2))
         thread_data[threadid].output->Output(fd, buf, count);
   }

   if (KnobEmulateSyscalls.Value() && thread_data[threadid].output)
   {
      switch(syscall_number)
      {
         // Handle SYS_clone child tid capture for proper pthread_join emulation.
         // When the CLONE_CHILD_CLEARTID option is enabled, remember its child_tidptr and
         // then when the thread ends, write 0 to the tid mutex and futex_wake it
         case SYS_clone:
         {
            thread_data[threadid].output->NewThread();
            // Store the thread's tid ptr for later use
            #if defined(TARGET_IA32)
               ADDRINT tidptr = args[2];
            #elif defined(TARGET_INTEL64)
               ADDRINT tidptr = args[3];
            #endif
            GetLock(&new_threadid_lock, threadid);
            tidptrs.push_back(tidptr);
            ReleaseLock(&new_threadid_lock);
            break;
         }

         // System calls not emulated (passed through to OS)
         case SYS_write:
            thread_data[threadid].last_syscall_number = syscall_number;
            thread_data[threadid].last_syscall_emulated = false;
            thread_data[threadid].output->Syscall(syscall_number, (char*)args, sizeof(args));
            break;

         // System calls emulated (not passed through to OS)
         case SYS_futex:
            thread_data[threadid].last_syscall_number = syscall_number;
            thread_data[threadid].last_syscall_emulated = true;
            thread_data[threadid].last_syscall_returnval = thread_data[threadid].output->Syscall(syscall_number, (char*)args, sizeof(args));
            break;

         // System calls sent to Sniper, but also passed through to OS
         case SYS_exit_group:
            thread_data[threadid].output->Syscall(syscall_number, (char*)args, sizeof(args));
            break;
      }
   }
}

void routineEnter(THREADID threadid, ADDRINT eip, ADDRINT esp)
{
   if (thread_data[threadid].output)
   {
      thread_data[threadid].output->RoutineChange(eip, esp, Sift::RoutineEnter);
      thread_data[threadid].last_routine = eip;
   }
}

void routineExit(THREADID threadid, ADDRINT eip, ADDRINT esp)
{
   if (thread_data[threadid].output)
   {
      thread_data[threadid].output->RoutineChange(eip, esp, Sift::RoutineExit);
      thread_data[threadid].last_routine = -1;
   }
}

void routineAssert(THREADID threadid, ADDRINT eip, ADDRINT esp)
{
   if (thread_data[threadid].output && thread_data[threadid].last_routine != eip)
   {
      thread_data[threadid].output->RoutineChange(eip, esp, Sift::RoutineAssert);
      thread_data[threadid].last_routine = eip;
   }
}

void announceRoutine(RTN rtn)
{
   if (!thread_data[PIN_ThreadId()].output)
      return;

   routines[RTN_Address(rtn)] = true;

   INT32 column = 0, line = 0;
   std::string filename = "??";
   PIN_GetSourceLocation(RTN_Address(rtn), &column, &line, &filename);
   IMG img = IMG_FindByAddress(RTN_Address(rtn));
   thread_data[PIN_ThreadId()].output->RoutineAnnounce(
      RTN_Address(rtn),
      RTN_Name(rtn).c_str(),
      IMG_Valid(img) ? IMG_Name(img).c_str() : "??",
      IMG_Valid(img) ? IMG_LoadOffset(img) : 0,
      column, line, filename.c_str());
}

void announceInvalidRoutine()
{
   if (!thread_data[PIN_ThreadId()].output)
      return;

   routines[0] = true;
   thread_data[PIN_ThreadId()].output->RoutineAnnounce(0, "INVALID", "", 0, 0, 0, "");
}

void addRtnTracer(RTN rtn)
{
   RTN_Open(rtn);

   if (routines.count(RTN_Address(rtn)) == 0)
      announceRoutine(rtn);

   RTN_InsertCall(rtn, IPOINT_BEFORE, AFUNPTR(routineEnter), IARG_THREAD_ID, IARG_ADDRINT, RTN_Address(rtn), IARG_REG_VALUE, REG_STACK_PTR, IARG_END);
   RTN_InsertCall(rtn, IPOINT_AFTER,  AFUNPTR(routineExit), IARG_THREAD_ID, IARG_ADDRINT, RTN_Address(rtn), IARG_REG_VALUE, REG_STACK_PTR, IARG_END);

   RTN_Close(rtn);
}

void addRtnTracer(TRACE trace)
{
   // At the start of each trace, check to see if this part of the code belongs to the function we think we're in.
   // This will detect longjmps and tail call elimination, and fix up the call stack appropriately.
   RTN rtn = TRACE_Rtn(trace);

   if (RTN_Valid(rtn))
   {
      if (routines.count(RTN_Address(rtn)) == 0)
         announceRoutine(rtn);

      TRACE_InsertCall(trace, IPOINT_BEFORE, AFUNPTR (routineAssert), IARG_THREAD_ID, IARG_ADDRINT, RTN_Address(rtn), IARG_REG_VALUE, REG_STACK_PTR, IARG_END);
   }
   else
   {
      if (routines.count(0) == 0)
         announceInvalidRoutine();

      TRACE_InsertCall(trace, IPOINT_BEFORE, AFUNPTR (routineAssert), IARG_THREAD_ID, IARG_ADDRINT, 0, IARG_REG_VALUE, REG_STACK_PTR, IARG_END);
   }
}

VOID traceCallback(TRACE trace, void *v)
{
   if (KnobRoutineTracing.Value())
      addRtnTracer(trace);

   BBL bbl_head = TRACE_BblHead(trace);

   for (BBL bbl = bbl_head; BBL_Valid(bbl); bbl = BBL_Next(bbl))
   {
      for(INS ins = BBL_InsHead(bbl); ; ins = INS_Next(ins))
      {
         // Simics-style magic instruction: xchg bx, bx
         if (INS_IsXchg(ins) && INS_OperandReg(ins, 0) == REG_BX && INS_OperandReg(ins, 1) == REG_BX)
         {
            INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)handleMagic, IARG_RETURN_REGS, REG_GAX, IARG_THREAD_ID, IARG_REG_VALUE, REG_GAX, IARG_REG_VALUE, REG_GBX, IARG_REG_VALUE, REG_GCX, IARG_END);
         }

         // Handle emulated syscalls
         if (INS_IsSyscall(ins))
         {
            INS_InsertPredicatedCall(ins, IPOINT_BEFORE, AFUNPTR(emulateSyscallFunc), IARG_THREAD_ID, IARG_CONST_CONTEXT, IARG_END);
         }

         if (ins == BBL_InsTail(bbl))
            break;
      }

      if (!any_thread_in_detail)
      {
         BBL_InsertCall(bbl, IPOINT_ANYWHERE, (AFUNPTR)countInsns, IARG_THREAD_ID, IARG_UINT32, BBL_NumIns(bbl), IARG_END);
      }
      else
      {
         for(INS ins = BBL_InsHead(bbl); ; ins = INS_Next(ins))
         {
            // For memory instructions, we should populate data items before we send the MicroOp
            UINT32 num_addresses = addMemoryModeling(ins);

            bool is_branch = INS_IsBranch(ins) && INS_HasFallThrough(ins);

            if (is_branch)
            {
               insertCall(ins, IPOINT_AFTER,        num_addresses, true  /* is_branch */, false /* taken */);
               insertCall(ins, IPOINT_TAKEN_BRANCH, num_addresses, true  /* is_branch */, true  /* taken */);
            }
            else
            {
               // Whenever possible, use IPOINT_AFTER as this allows us to process addresses after the application has used them.
               // This ensures that their logical to physical mapping has been set up.
               insertCall(ins, INS_HasFallThrough(ins) ? IPOINT_AFTER : IPOINT_BEFORE, num_addresses, false /* is_branch */, false /* taken */);
            }

            if (ins == BBL_InsTail(bbl))
               break;
         }
      }
   }
}

VOID syscallEntryCallback(THREADID threadid, CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard, VOID *v)
{
   if (!thread_data[threadid].last_syscall_emulated)
   {
      return;
   }

   PIN_SetSyscallNumber(ctxt, syscall_standard, SYS_getpid);
}

VOID syscallExitCallback(THREADID threadid, CONTEXT *ctxt, SYSCALL_STANDARD syscall_standard, VOID *v)
{
   if (!thread_data[threadid].last_syscall_emulated)
   {
      return;
   }

   PIN_SetContextReg(ctxt, REG_GAX, thread_data[threadid].last_syscall_returnval);
   thread_data[threadid].last_syscall_emulated = false;
}

VOID Fini(INT32 code, VOID *v)
{
   for (unsigned int i = 0 ; i < MAX_NUM_THREADS ; i++)
   {
      if (thread_data[i].output)
      {
         closeFile(i);
      }
   }
}

VOID Detach(VOID *v)
{
}

void getCode(uint8_t *dst, const uint8_t *src, uint32_t size)
{
   PIN_SafeCopy(dst, src, size);
}

void handleAccessMemory(void *arg, Sift::MemoryLockType lock_signal, Sift::MemoryOpType mem_op, uint64_t d_addr, uint8_t* data_buffer, uint32_t data_size)
{
   // Lock memory globally if requested
   // This operation does not occur very frequently, so this should not impact performance
   if (lock_signal == Sift::MemLock)
   {
      GetLock(&access_memory_lock, 0);
   }

   if (mem_op == Sift::MemRead)
   {
      // The simulator is requesting data from us
      PIN_SafeCopy(data_buffer, reinterpret_cast<void*>(d_addr), data_size);
   }
   else if (mem_op == Sift::MemWrite)
   {
      // The simulator is requesting that we write data back to memory
      PIN_SafeCopy(reinterpret_cast<void*>(d_addr), data_buffer, data_size);
   }
   else
   {
      std::cerr << "Error: invalid memory operation type" << std::endl;
      sift_assert(false);
   }

   if (lock_signal == Sift::MemUnlock)
   {
      ReleaseLock(&access_memory_lock);
   }
}

void openFile(THREADID threadid)
{
   if (thread_data[threadid].output)
   {
      closeFile(threadid);
      ++thread_data[threadid].blocknum;
   }

   if (thread_data[threadid].thread_num != 0)
   {
      sift_assert(KnobUseResponseFiles.Value() != 0);
   }

   char filename[1024] = {0};
   char response_filename[1024] = {0};
   if (KnobUseResponseFiles.Value() == 0)
   {
      if (blocksize)
         sprintf(filename, "%s.%" PRIu64 ".sift", KnobOutputFile.Value().c_str(), thread_data[threadid].blocknum);
      else
         sprintf(filename, "%s.sift", KnobOutputFile.Value().c_str());
   }
   else
   {
      if (blocksize)
         sprintf(filename, "%s.%" PRIu64 ".app%" PRId32 ".th%" PRIu64 ".sift", KnobOutputFile.Value().c_str(), thread_data[threadid].blocknum, app_id, thread_data[threadid].thread_num);
      else
         sprintf(filename, "%s.app%" PRId32 ".th%" PRIu64 ".sift", KnobOutputFile.Value().c_str(), app_id, thread_data[threadid].thread_num);
   }

   if (verbose)
      std::cerr << "[SIFT_RECORDER:" << app_id << ":" << thread_data[threadid].thread_num << "] Output = [" << filename << "]" << std::endl;

   if (KnobUseResponseFiles.Value())
   {
      sprintf(response_filename, "%s_response.app%" PRId32 ".th%" PRIu64 ".sift", KnobOutputFile.Value().c_str(), app_id, thread_data[threadid].thread_num);
      if (verbose)
         std::cerr << "[SIFT_RECORDER:" << app_id << ":" << thread_data[threadid].thread_num << "] Response = [" << response_filename << "]" << std::endl;
   }


   // Open the file for writing
   try {
      #ifdef TARGET_IA32
         const bool arch32 = true;
      #else
         const bool arch32 = false;
      #endif
      thread_data[threadid].output = new Sift::Writer(filename, getCode, false, response_filename, threadid, arch32, false, KnobSendPhysicalAddresses.Value());
   } catch (...) {
      std::cerr << "[SIFT_RECORDER:" << app_id << ":" << thread_data[threadid].thread_num << "] Error: Unable to open the output file " << filename << std::endl;
      exit(1);
   }

   thread_data[threadid].output->setHandleAccessMemoryFunc(handleAccessMemory, reinterpret_cast<void*>(threadid));
}

void closeFile(THREADID threadid)
{
   if (verbose)
   {
      std::cerr << "[SIFT_RECORDER:" << app_id << ":" << thread_data[threadid].thread_num << "] Recorded " << thread_data[threadid].icount_detailed;
      if (thread_data[threadid].icount > thread_data[threadid].icount_detailed)
         std::cerr << " (out of " << thread_data[threadid].icount << ")";
      std::cerr << " instructions" << std::endl;
   }

   Sift::Writer *output = thread_data[threadid].output;
   thread_data[threadid].output = NULL;
   // Thread will stop writing to output from this point on
   output->End();
   delete output;

   if (blocksize)
   {
      if (thread_data[threadid].bbv_count)
      {
         thread_data[threadid].bbv->count(thread_data[threadid].bbv_base, thread_data[threadid].bbv_count);
         thread_data[threadid].bbv_base = 0; // Next instruction starts a new basic block
         thread_data[threadid].bbv_count = 0;
      }

      char filename[1024];
      sprintf(filename, "%s.%" PRIu64 ".bbv", KnobOutputFile.Value().c_str(), thread_data[threadid].blocknum);

      FILE *fp = fopen(filename, "w");
      fprintf(fp, "%" PRIu64 "\n", thread_data[threadid].bbv->getInstructionCount());
      for(int i = 0; i < Bbv::NUM_BBV; ++i)
         fprintf(fp, "%" PRIu64 "\n", thread_data[threadid].bbv->getDimension(i) / thread_data[threadid].bbv->getInstructionCount());
      fclose(fp);

      thread_data[threadid].bbv->clear();
   }
}

// The thread that watched this new thread start is responsible for setting up the connection with the simulator
VOID threadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
   sift_assert(thread_data[threadid].bbv == NULL);
   sift_assert(thread_data[threadid].dyn_address_queue == NULL);

   // The first thread (master) doesn't need to join with anyone else
   GetLock(&new_threadid_lock, threadid);
   if (tidptrs.size() > 0)
   {
      thread_data[threadid].tid_ptr = tidptrs.front();
      tidptrs.pop_front();
   }
   ReleaseLock(&new_threadid_lock);

   thread_data[threadid].thread_num = num_threads++;
   thread_data[threadid].bbv = new Bbv();
   thread_data[threadid].dyn_address_queue = new std::deque<ADDRINT>();

   if (threadid > 0 && (any_thread_in_detail || KnobEmulateSyscalls.Value()))
      openFile(threadid);

   thread_data[threadid].running = true;
}

VOID threadFinishHelper(VOID *arg)
{
   uint64_t threadid = reinterpret_cast<uint64_t>(arg);
   if (thread_data[threadid].tid_ptr)
   {
      // Set this pointer to 0 to indicate that this thread is complete
      intptr_t tid = (intptr_t)thread_data[threadid].tid_ptr;
      *(int*)tid = 0;
      // Send the FUTEX_WAKE to the simulator to wake up a potential pthread_join() caller
      syscall_args_t args = {0};
      args[0] = (intptr_t)tid;
      args[1] = FUTEX_WAKE;
      args[2] = 1;
      thread_data[threadid].output->Syscall(SYS_futex, (char*)args, sizeof(args));
   }

   if (thread_data[threadid].output)
   {
      closeFile(threadid);
   }

   delete thread_data[threadid].dyn_address_queue;
   delete thread_data[threadid].bbv;

   thread_data[threadid].dyn_address_queue = NULL;
   thread_data[threadid].bbv = NULL;
}

VOID threadFinish(THREADID threadid, const CONTEXT *ctxt, INT32 flags, VOID *v)
{
#if DEBUG_OUTPUT
   std::cerr << "[SIFT_RECORDER:" << app_id << ":" << thread_data[threadid].thread_num << "] Finish Thread" << std::endl;
#endif

   if (thread_data[threadid].thread_num == 0 && thread_data[threadid].output && KnobEmulateSyscalls.Value())
   {
      // Send SYS_exit_group to the simulator to end the application
      syscall_args_t args = {0};
      args[0] = flags;
      thread_data[threadid].output->Syscall(SYS_exit_group, (char*)args, sizeof(args));
   }

   thread_data[threadid].running = false;

   // To prevent deadlocks during simulation, start a new thread to handle this thread's
   // cleanup.  This is needed because this function could be called in the context of
   // another thread, creating a deadlock scenario.
   PIN_SpawnInternalThread(threadFinishHelper, (VOID*)(unsigned long)threadid, 0, NULL);
}

void findMyAppId()
{
   // FIFOs for thread 0 of each application have been created beforehand
   // Protocol to figure out our application id is to lock a .app file
   // Try to lock from app_id 0 onwards, claiming the app_id if the lock succeeds,
   // and fail when there are no more files
   for(uint64_t id = 0; id < 1000; ++id)
   {
      char filename[1024] = {0};

      // First check whether this many appids are supported by stat()ing the request FIFO for thread 0
      struct stat sts;
      sprintf(filename, "%s.app%" PRIu64 ".th%" PRIu64 ".sift", KnobOutputFile.Value().c_str(), id, (uint64_t)0);
      if (stat(filename, &sts) != 0)
      {
         break;
      }

      // Atomically create .appid file
      sprintf(filename, "%s.app%" PRIu64 ".appid", KnobOutputFile.Value().c_str(), id);
      int fd = open(filename, O_CREAT | O_EXCL, 0600);
      if (fd != -1)
      {
         // Success: use this app_id
         app_id = id;
         std::cerr << "[SIFT_RECORDER:" << app_id << "] Application started" << std::endl;
         return;
      }
      // Could not create, probably someone else raced us to it. Try next app_id
   }
   std::cerr << "[SIFT_RECORDER] Cannot find free application id, too many processes!" << std::endl;
   exit(1);
}

BOOL followChild(CHILD_PROCESS childProcess, VOID *val)
{
   if (any_thread_in_detail)
   {
      fprintf(stderr, "EXECV ignored while in ROI\n");
      return false; // Cannot fork/execv after starting ROI
   }
   else
      return true;
}

VOID forkBefore(THREADID threadid, const CONTEXT *ctxt, VOID *v)
{
   sift_assert(!any_thread_in_detail); // Cannot fork after starting ROI
}

VOID handleRoutineImplicitROI(THREADID threadid, bool begin)
{
   if (begin)
      beginROI(threadid);
   else
      endROI(threadid);
}

void routineCallback(RTN rtn, void* v)
{
   if (KnobRoutineTracing.Value())
      addRtnTracer(rtn);

   if (KnobMPIImplicitROI.Value())
   {
      std::string rtn_name = RTN_Name(rtn);
      if (rtn_name.find("MPI_Init") != string::npos && rtn_name.find("MPI_Initialized") == string::npos) // Actual name can be MPI_Init, MPI_Init_thread, PMPI_Init_thread, etc.
      {
         RTN_Open(rtn);
         RTN_InsertCall(rtn, IPOINT_AFTER, AFUNPTR(handleRoutineImplicitROI), IARG_THREAD_ID, IARG_BOOL, true, IARG_END);
         RTN_Close(rtn);
      }
      if (rtn_name == "MPI_Finalize")
      {
         RTN_Open(rtn);
         RTN_InsertCall(rtn, IPOINT_BEFORE, AFUNPTR(handleRoutineImplicitROI), IARG_THREAD_ID, IARG_BOOL, false, IARG_END);
         RTN_Close(rtn);
      }
   }
}

bool assert_ignore()
{
   struct stat st;
   if (stat((KnobOutputFile.Value() + ".sift_done").c_str(), &st) == 0)
      return true;
   else
      return false;
}

void __sift_assert_fail(__const char *__assertion, __const char *__file,
                        unsigned int __line, __const char *__function)
     __THROW
{
   if (assert_ignore())
   {
      // Timing model says it's done, ignore assert and pretend to have exited cleanly
      exit(0);
   }
   else
   {
      std::cerr << "[SIFT_RECORDER] " << __file << ":" << __line << ": " << __function
                << ": Assertion `" << __assertion << "' failed." << std::endl;
      abort();
   }
}

int main(int argc, char **argv)
{
   if (PIN_Init(argc,argv))
   {
      std::cerr << "Error, invalid parameters" << std::endl;
      std::cerr << KNOB_BASE::StringKnobSummary() << std::endl;
      exit(1);
   }
   PIN_InitSymbols();

   size_t thread_data_size = MAX_NUM_THREADS * sizeof(*thread_data);
   if (posix_memalign((void**)&thread_data, LINE_SIZE_BYTES, thread_data_size) != 0)
   {
      std::cerr << "Error, posix_memalign() failed" << std::endl;
      exit(1);
   }
   bzero(thread_data, thread_data_size);

   InitLock(&access_memory_lock);
   InitLock(&new_threadid_lock);

   app_id = KnobSiftAppId.Value();
   blocksize = KnobBlocksize.Value();
   fast_forward_target = KnobFastForwardTarget.Value();
   detailed_target = KnobDetailedTarget.Value();
   if (KnobEmulateSyscalls.Value() || (!KnobUseROI.Value() && !KnobMPIImplicitROI.Value()))
   {
      if (app_id < 0)
         findMyAppId();
   }
   if (fast_forward_target == 0 && !KnobUseROI.Value() && !KnobMPIImplicitROI.Value())
   {
      any_thread_in_detail = true;
      openFile(0);
   }
   if (KnobEmulateSyscalls.Value())
   {
      openFile(0);
   }

#ifdef PINPLAY_SUPPORTED
   if (KnobReplayer.Value())
   {
      if (KnobEmulateSyscalls.Value())
      {
         std::cerr << "Error, emulating syscalls is not compatible with PinPlay replaying." << std::endl;
         exit(1);
      }
      pinplay_engine.Activate(argc, argv, false /*logger*/, KnobReplayer.Value() /*replayer*/);
   }
#else
   if (KnobReplayer.Value())
   {
      std::cerr << "Error, PinPlay support not compiled in. Please use a compatible pin kit when compiling." << std::endl;
      exit(1);
   }
#endif

   if (KnobEmulateSyscalls.Value())
   {
      if (!KnobUseResponseFiles.Value())
      {
         std::cerr << "Error, Response files are required when using syscall emulation." << std::endl;
         exit(1);
      }

      PIN_AddSyscallEntryFunction(syscallEntryCallback, 0);
      PIN_AddSyscallExitFunction(syscallExitCallback, 0);
   }

   TRACE_AddInstrumentFunction(traceCallback, 0);
   RTN_AddInstrumentFunction(routineCallback, 0);

   PIN_AddThreadStartFunction(threadStart, 0);
   PIN_AddThreadFiniFunction(threadFinish, 0);
   PIN_AddFiniFunction(Fini, 0);
   PIN_AddDetachFunction(Detach, 0);

   PIN_AddFollowChildProcessFunction(followChild, 0);
   PIN_AddForkFunction(FPOINT_BEFORE, forkBefore, 0);

   if (KnobDebug.Value())
      pinboost_register("SIFT_RECORDER");

   PIN_StartProgram();

   return 0;
}
