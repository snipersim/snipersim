#include "recorder_base.h"
#include "globals.h"
#include "threads.h"
#include "sift_assert.h"
#include "recorder_control.h"
#include "syscall_modeling.h"

#include <iostream>

VOID countInsns(THREADID threadid, INT32 count)
{
   thread_data[threadid].icount += count;

   if (thread_data[threadid].icount >= fast_forward_target && !KnobUseROI.Value() && !KnobMPIImplicitROI.Value())
   {
      if (KnobVerbose.Value())
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

static VOID traceCallback(TRACE trace, void *v)
{
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

void initRecorderBase()
{
   TRACE_AddInstrumentFunction(traceCallback, 0);
}
