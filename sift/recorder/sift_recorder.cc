#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <syscall.h>
//#include <unordered_map>
#include <vector>
#include <deque>

#include <cstdio>
#include <cassert>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "pin.H"

#include "sift_writer.h"
#include "bbv_count.h"

//#define DEBUG_OUTPUT 1
#define DEBUG_OUTPUT 0

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool", "o", "trace", "output");
KNOB<UINT64> KnobBlocksize(KNOB_MODE_WRITEONCE, "pintool", "b", "0", "blocksize");
KNOB<UINT64> KnobFastForwardTarget(KNOB_MODE_WRITEONCE, "pintool", "f", "0", "instructions to fast forward");
KNOB<UINT64> KnobDetailedTarget(KNOB_MODE_WRITEONCE, "pintool", "d", "0", "instructions to trace in detail (default = all)");

BOOL in_detail = false;
UINT64 blocksize;
UINT64 fast_forward_target = 0;
UINT64 detailed_target = 0;
UINT64 blocknum = 0;
UINT64 icount = 0;
UINT64 icount_detailed = 0;
std::deque<ADDRINT> dyn_address_queue;

Sift::Writer *output;
Bbv bbv;
ADDRINT bbv_base = 0;
UINT64 bbv_count = 0;

void openFile();
void closeFile();

VOID countInsns(THREADID thread_id, INT32 count)
{
   // TODO: support multi-threading
   assert(thread_id == 0);

   icount += count;

   if (icount >= fast_forward_target)
   {
      std::cout << "[SIFT_RECORDER] Changing to detailed after " << icount << " instructions" << std::endl;
      in_detail = true;
      icount = 0;
      PIN_RemoveInstrumentation();
   }
}

VOID sendInstruction(THREADID thread_id, ADDRINT addr, UINT32 size, UINT32 num_addresses, BOOL is_branch, BOOL taken, BOOL is_predicate, BOOL executing)
{
   // TODO: support multi-threading
   assert(thread_id == 0);

   ++icount;
   ++icount_detailed;

   if (bbv_base == 0)
   {
      bbv_base = addr; // We're the start of a new basic block
   }
   bbv_count++;
   if (is_branch)
   {
      bbv.count(bbv_base, bbv_count);
      bbv_base = 0; // Next instruction starts a new basic block
      bbv_count = 0;
   }

   intptr_t addresses[Sift::MAX_DYNAMIC_ADDRESSES] = { 0 };
   for(uint8_t i = 0; i < num_addresses; ++i)
   {
      addresses[i] = dyn_address_queue.front();
      assert(!dyn_address_queue.empty());
      dyn_address_queue.pop_front();
   }
   assert(dyn_address_queue.empty());

   output->Instruction(addr, size, num_addresses, addresses, is_branch, taken, is_predicate, executing);

   if (detailed_target != 0 && icount_detailed >= detailed_target)
   {
      PIN_Detach();
      return;
   }

   if (blocksize && icount >= blocksize)
   {
      openFile();
      icount = 0;
   }
}

VOID handleMemory(THREADID thread_id, ADDRINT address)
{
   dyn_address_queue.push_back(address);
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
   assert(num_addresses <= Sift::MAX_DYNAMIC_ADDRESSES);

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
      IARG_END);
}

VOID traceCallback(TRACE trace, void *v)
{
   BBL bbl_head = TRACE_BblHead(trace);

   for (BBL bbl = bbl_head; BBL_Valid(bbl); bbl = BBL_Next(bbl))
   {
      if (!in_detail)
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
               insertCall(ins, IPOINT_BEFORE,       num_addresses, false /* is_branch */, false /* taken */);

            if (ins == BBL_InsTail(bbl))
               break;
         }
      }
   }
}

VOID syscallEntryCallback(THREADID threadIndex, CONTEXT *ctxt, SYSCALL_STANDARD std, VOID *v)
{
   if (!in_detail)
      return;

   ADDRINT syscall_number = PIN_GetContextReg(ctxt, REG_GAX);
   if (syscall_number == SYS_write) {
      int fd = (int)PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GDI);
      const char *buf = (const char*)PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GSI);
      size_t count = (size_t)PIN_GetContextReg(ctxt, LEVEL_BASE::REG_GDX);

      if (count > 0 && (fd == 1 || fd == 2))
         output->Output(fd, buf, count);
   }
}

VOID TheEnd()
{
   closeFile();
}

VOID Fini(INT32 code, VOID *v)
{
   TheEnd();
}

VOID Detach(VOID *v)
{
   TheEnd();
}

void getCode(uint8_t *dst, const uint8_t *src, uint32_t size)
{
   PIN_SafeCopy(dst, src, size);
}

void openFile()
{
   if (output)
   {
      closeFile();
      ++blocknum;
   }

   char filename[1024];
   if (blocksize)
      sprintf(filename, "%s.%lu.sift", KnobOutputFile.Value().c_str(), blocknum);
   else
      sprintf(filename, "%s.sift", KnobOutputFile.Value().c_str());

   std::cout << "[SIFT_RECORDER] Output = [" << filename << "]" << std::endl;

   // Open the file for writing
   try {
      output = new Sift::Writer(filename, getCode);
   } catch (...) {
      std::cout << "[SIFT_RECORDER] Error: Unable to open the output file " << filename << std::endl;
      exit(1);
   }
}

void closeFile()
{
   delete output;

   if (blocksize)
   {
      if (bbv_count)
      {
         bbv.count(bbv_base, bbv_count);
         bbv_base = 0; // Next instruction starts a new basic block
         bbv_count = 0;
      }

      char filename[1024];
      sprintf(filename, "%s.%lu.bbv", KnobOutputFile.Value().c_str(), blocknum);

      FILE *fp = fopen(filename, "w");
      fprintf(fp, "%lu\n", bbv.getInstructionCount());
      for(int i = 0; i < Bbv::NUM_BBV; ++i)
         fprintf(fp, "%lu\n", bbv.getDimension(i) / bbv.getInstructionCount());
      fclose(fp);

      bbv.clear();
   }
}

int main(int argc, char **argv)
{
   if (PIN_Init(argc,argv)) {
      std::cerr << "Error, invalid parameters" << std::endl;
      exit(1);
   }

   blocksize = KnobBlocksize.Value();
   fast_forward_target = KnobFastForwardTarget.Value();
   detailed_target = KnobDetailedTarget.Value();
   if (fast_forward_target == 0)
      in_detail = true;

   openFile();

   TRACE_AddInstrumentFunction(traceCallback, 0);
   PIN_AddSyscallEntryFunction(syscallEntryCallback, 0);
   PIN_AddFiniFunction(Fini, 0);
   PIN_AddDetachFunction(Detach, 0);

   PIN_StartProgram();

   return 0;
}
