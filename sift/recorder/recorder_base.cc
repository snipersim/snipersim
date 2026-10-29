#include "recorder_base.h"
#include "globals.h"
#include "threads.h"
#include "timed_calls.h"
#include "sift_assert.h"
#include "recorder_control.h"
#include "syscall_modeling.h"
#include <iostream>

#define DEBUG

VOID writeToVMATrace(THREADID threadid)
{
   if (!vma_output)
   {
      return;
   }
   if (!(thread_data[threadid].icount_detailed % 10000 == 0))
   {
      return;
   }

   if (KnobVerbose.Value())
      std::cerr << "[SIFT_RECORDER:" << app_id << ":" << thread_data[threadid].thread_num << "] Recording VMAs for app" << thread_data[threadid].icount_detailed << " instructions" << std::endl;

   pid_t pid = PIN_GetPid();

   // Construct the path to the /proc/[pid]/maps file
   std::ostringstream path;
   path << "/proc/" << pid << "/maps";

   // Open the maps file for reading
   std::ifstream inFile(path.str().c_str());
   if (!inFile)
   {
      std::cerr << "Unable to open maps file for reading!" << std::endl;
      return;
   }

   *vma_output << "VMA: " << thread_data[threadid].icount_detailed << " " << std::endl;

   std::string line;
   while (std::getline(inFile, line))
   {
      size_t hyphenPos = line.find('-');
      if (hyphenPos != std::string::npos)
      {
         size_t spacePos = line.find(' ', hyphenPos);
         *vma_output << line.substr(0, spacePos) << std::endl;
      }
   }

}

VOID countInsns(THREADID threadid, INT32 count)
{
   thread_data[threadid].icount += count;
   thread_data[threadid].icount_total += count;

   if (!any_thread_in_detail && thread_data[threadid].output)
   {
      thread_data[threadid].icount_reported += count;
      if (thread_data[threadid].icount_reported > KnobFlowControlFF.Value())
      {
         Sift::Mode mode = thread_data[threadid].output->InstructionCount(thread_data[threadid].icount_reported);
         thread_data[threadid].icount_reported = 0;
         setInstrumentationMode(mode);
         timedFlow(threadid);   /* live: the guest's timers (timed_calls.cc) */
      }
   }

   if (thread_data[threadid].icount >= fast_forward_target && !in_roi && !KnobUseROI.Value() && !KnobMPIImplicitROI.Value())
   {
      if (KnobVerbose.Value())
         std::cerr << "[SIFT_RECORDER:" << app_id << ":" << thread_data[threadid].thread_num << "] Changing to detailed after " << thread_data[threadid].icount << " instructions" << std::endl;
      if (!thread_data[threadid].output)
         openFile(threadid);
      thread_data[threadid].icount = 0;
      in_roi = true;
      setInstrumentationMode(Sift::ModeDetailed);
   }
}

VOID sendInstruction(THREADID threadid, ADDRINT addr, UINT32 size, UINT32 num_addresses, BOOL is_branch, BOOL taken, BOOL is_predicate, BOOL executing, BOOL isbefore, BOOL ispause)
{
   // We're still called for instructions in the same basic block as ROI end, ignore these
   writeToVMATrace(threadid);
   ++thread_data[threadid].icount_total;

   if (!thread_data[threadid].output)
   {
      thread_data[threadid].num_dyn_addresses = 0;
      return;
   }

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

   sift_assert(thread_data[threadid].num_dyn_addresses == num_addresses);
   if (isbefore)
   {
      for (uint8_t i = 0; i < num_addresses; ++i)
      {
         // If the instruction hasn't executed yet, access the address to ensure a page fault if the mapping wasn't set up yet
         static char dummy = 0;
         dummy += *(char *)translateAddress(thread_data[threadid].dyn_addresses[i], 0);
      }
   }

   thread_data[threadid].output->Instruction(addr, size, num_addresses, thread_data[threadid].dyn_addresses, is_branch, taken, is_predicate, executing);
   thread_data[threadid].num_dyn_addresses = 0;

   if (KnobUseResponseFiles.Value() && KnobFlowControl.Value() && (thread_data[threadid].icount > thread_data[threadid].flowcontrol_target || ispause))
   {
      Sift::Mode mode = thread_data[threadid].output->Sync();
      thread_data[threadid].flowcontrol_target = thread_data[threadid].icount + KnobFlowControl.Value();
      setInstrumentationMode(mode);
      timedFlow(threadid);   /* live: the guest's timers (timed_calls.cc) */
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

VOID cacheOnlyUpdateInsCount(THREADID threadid, UINT32 icount)
{
   thread_data[threadid].icount_cacheonly_pending += icount;
   thread_data[threadid].icount_total += icount;

   /* The pending count normally rides along with the next memory access.
      A stretch without one -- a compute loop -- sends nothing, so the
      simulator's clock for this core stands still while the thread runs on
      natively; with the barrier active in warm-up mode every other core
      then waits for this one, and on a sampled run (Pac-Sim) the regions
      cut by the running thread contained it alone.  So once the count
      outgrows the flow-control window it goes out as an instruction count,
      which advances the clock and, being answered, holds the thread to the
      simulator's pace like any other report. */
   if (thread_data[threadid].icount_cacheonly_pending > KnobFlowControlFF.Value() && thread_data[threadid].output)
   {
      Sift::Mode mode = thread_data[threadid].output->InstructionCount(thread_data[threadid].icount_cacheonly_pending);
      thread_data[threadid].icount_cacheonly += thread_data[threadid].icount_cacheonly_pending;
      thread_data[threadid].icount_cacheonly_pending = 0;
      thread_data[threadid].icount_reported = 0;
      setInstrumentationMode(mode);
      timedFlow(threadid);   /* live: the guest's timers (timed_calls.cc) */
   }
}

VOID cacheOnlyConsumeAddresses(THREADID threadid)
{
   thread_data[threadid].num_dyn_addresses = 0;
}

VOID sendCacheOnly(THREADID threadid, UINT32 icount, UINT32 type, ADDRINT eip, ADDRINT arg)
{
   // We're still called for instructions in the same basic block as ROI end, ignore these
   if (!thread_data[threadid].output)
      return;

   cacheOnlyUpdateInsCount(threadid, icount);

   ADDRINT address;
   switch (Sift::CacheOnlyType(type))
   {
   case Sift::CacheOnlyMemRead:
   case Sift::CacheOnlyMemWrite:
      address = thread_data[threadid].dyn_addresses[arg];
      break;
   default:
      address = arg;
      break;
   }
   thread_data[threadid].output->CacheOnly(thread_data[threadid].icount_cacheonly_pending, Sift::CacheOnlyType(type), eip, address);

   thread_data[threadid].icount_cacheonly += thread_data[threadid].icount_cacheonly_pending;
   thread_data[threadid].icount_reported += thread_data[threadid].icount_cacheonly_pending;
   thread_data[threadid].icount_cacheonly_pending = 0;

   if (thread_data[threadid].icount_reported > KnobFlowControlFF.Value())
   {
      Sift::Mode mode = thread_data[threadid].output->Sync();
      thread_data[threadid].icount_reported = 0;
      setInstrumentationMode(mode);
      timedFlow(threadid);   /* live: the guest's timers (timed_calls.cc) */
   }
}

VOID handleMemory(THREADID threadid, ADDRINT address)
{
   // We're still called for instructions in the same basic block as ROI end, ignore these
   if (!thread_data[threadid].output)
      return;

   thread_data[threadid].dyn_addresses[thread_data[threadid].num_dyn_addresses++] = address;
}



UINT32 addMemoryModeling(INS ins)
{
   UINT32 num_addresses = 0;

   if (INS_IsMemoryRead(ins) || INS_IsMemoryWrite(ins))
   {
      UINT32 max_op_count = std::min<UINT32>(INS_MemoryOperandCount(ins), Sift::MAX_DYNAMIC_ADDRESSES);
      for (unsigned int i = 0; i < max_op_count; i++)
      {
         INS_InsertCall(ins, IPOINT_BEFORE,
                        AFUNPTR(handleMemory),
                        IARG_THREAD_ID,
                        IARG_MEMORYOP_EA, i,
                        IARG_END);
         num_addresses++;
      }
   }
   if (INS_MemoryOperandCount(ins) > Sift::MAX_DYNAMIC_ADDRESSES)
   {
      std::cerr << "[SIFT_RECORDER] Unable to report all dynamic addresses (" << Sift::MAX_DYNAMIC_ADDRESSES << "/" << INS_MemoryOperandCount(ins) << ") for instruction 0x" << std::hex << INS_Address(ins) << std::dec << "\n";
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
   // to not add overhead when extrae is linked, we must ignore extrae instr.
   if (extrae_image.linked)
   {
      ADDRINT trace_address = TRACE_Address(trace);
      if (trace_address >= extrae_image.top_addr &&
          trace_address < extrae_image.bottom_addr)
      {
         return;
      }
   }

   BBL bbl_head = TRACE_BblHead(trace);

   for (BBL bbl = bbl_head; BBL_Valid(bbl); bbl = BBL_Next(bbl))
   {
      for (INS ins = BBL_InsHead(bbl);; ins = INS_Next(ins))
      {
         // Pin re-translates code that has been written since it was last
         // translated: tell the writers when the bytes differ from the ones
         // they sent (Sift::CodeTracker).  Translation time only.
         if (Sift::CodeTracker::enabled())
         {
            uint8_t bytes[16];
            size_t got = PIN_SafeCopy(bytes, reinterpret_cast<void*>(INS_Address(ins)), INS_Size(ins));
            Sift::CodeTracker::observe(INS_Address(ins), bytes, got);
         }

         // Simics-style magic instruction: xchg bx, bx
         if (INS_IsXchg(ins) && INS_OperandReg(ins, 0) == REG_BX && INS_OperandReg(ins, 1) == REG_BX)
         {
            INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)handleMagic, IARG_RETURN_REGS, REG_GAX, IARG_THREAD_ID, IARG_CONTEXT, IARG_REG_VALUE, REG_GAX,
#ifdef TARGET_IA32
                                     IARG_REG_VALUE, REG_GDX,
#else
                                     IARG_REG_VALUE, REG_GBX,
#endif
                                     IARG_REG_VALUE, REG_GCX, IARG_END);
         }

         // Handle emulated syscalls
         if (INS_IsSyscall(ins))
         {
            INS_InsertPredicatedCall(ins, IPOINT_BEFORE, AFUNPTR(emulateSyscallFunc), IARG_THREAD_ID, IARG_CONST_CONTEXT, IARG_END);
         }

         if (KnobStopAddress && (INS_Address(ins) == KnobStopAddress))
         {
            INS_InsertPredicatedCall(ins, IPOINT_BEFORE, AFUNPTR(endROI), IARG_THREAD_ID, IARG_END);
         }

         if (ins == BBL_InsTail(bbl))
            break;
      }

      if (!any_thread_in_detail)
      {
         BBL_InsertCall(bbl, IPOINT_ANYWHERE, (AFUNPTR)countInsns, IARG_THREAD_ID, IARG_UINT32, BBL_NumIns(bbl), IARG_END);
      }
      else if (current_mode == Sift::ModeDetailed)
      {
         for (INS ins = BBL_InsHead(bbl);; ins = INS_Next(ins))
         {
            // For memory instructions, collect all addresses at IPOINT_BEFORE
            UINT32 num_addresses = addMemoryModeling(ins);

            bool is_branch = INS_IsBranch(ins) && INS_HasFallThrough(ins);

            if (is_branch && INS_IsValidForIpointTakenBranch(ins) && INS_IsValidForIpointAfter(ins))
            {
               insertCall(ins, IPOINT_AFTER, num_addresses, true /* is_branch */, false /* taken */);
               insertCall(ins, IPOINT_TAKEN_BRANCH, num_addresses, true /* is_branch */, true /* taken */);
            }
            else
            {
               // Whenever possible, use IPOINT_AFTER as this allows us to process addresses after the application has used them.
               // This ensures that their logical to physical mapping has been set up.
               insertCall(ins, INS_IsValidForIpointAfter(ins) ? IPOINT_AFTER : IPOINT_BEFORE, num_addresses, false /* is_branch */, false /* taken */);
            }

            if (ins == BBL_InsTail(bbl))
               break;
         }
      }
      else if (current_mode == Sift::ModeMemory)
      {
         UINT32 inscount = 0;

         for (INS ins = BBL_InsHead(bbl);; ins = INS_Next(ins))
         {
            ++inscount;

            // For memory instructions, collect all addresses at IPOINT_BEFORE
            addMemoryModeling(ins);

            if (INS_IsMemoryRead(ins) || INS_IsMemoryWrite(ins))
            {
               // Prefer IPOINT_AFTER to maximize probability of physical mapping to be available
               IPOINT ipoint = INS_HasFallThrough(ins) ? IPOINT_AFTER : IPOINT_BEFORE;
               for (unsigned int idx = 0; idx < INS_MemoryOperandCount(ins); ++idx)
               {
                  if (INS_MemoryOperandIsRead(ins, idx))
                  {
                     INS_InsertCall(ins, ipoint,
                                    AFUNPTR(sendCacheOnly),
                                    IARG_THREAD_ID,
                                    IARG_UINT32, inscount,
                                    IARG_UINT32, UINT32(Sift::CacheOnlyMemRead),
                                    IARG_ADDRINT, INS_Address(ins),
                                    IARG_UINT32, UINT32(idx),
                                    IARG_END);
                     inscount = 0;
                  }
                  if (INS_MemoryOperandIsWritten(ins, idx))
                  {
                     INS_InsertCall(ins, ipoint,
                                    AFUNPTR(sendCacheOnly),
                                    IARG_THREAD_ID,
                                    IARG_UINT32, inscount,
                                    IARG_UINT32, UINT32(Sift::CacheOnlyMemWrite),
                                    IARG_ADDRINT, INS_Address(ins),
                                    IARG_UINT32, UINT32(idx),
                                    IARG_END);
                     inscount = 0;
                  }
               }
               INS_InsertCall(ins, ipoint,
                              AFUNPTR(cacheOnlyConsumeAddresses),
                              IARG_THREAD_ID,
                              IARG_END);
            }
            if (INS_IsBranch(ins) && INS_HasFallThrough(ins))
            {
               INS_InsertCall(ins, IPOINT_TAKEN_BRANCH,
                              AFUNPTR(sendCacheOnly),
                              IARG_THREAD_ID,
                              IARG_UINT32, inscount,
                              IARG_UINT32, UINT32(Sift::CacheOnlyBranchTaken),
                              IARG_ADDRINT, INS_Address(ins),
                              IARG_BRANCH_TARGET_ADDR,
                              IARG_END);
               INS_InsertCall(ins, IPOINT_AFTER,
                              AFUNPTR(sendCacheOnly),
                              IARG_THREAD_ID,
                              IARG_UINT32, inscount,
                              IARG_UINT32, Sift::CacheOnlyBranchNotTaken,
                              IARG_ADDRINT, INS_Address(ins),
                              IARG_FALLTHROUGH_ADDR,
                              IARG_END);
               inscount = 0;
            }

            if (ins == BBL_InsTail(bbl))
            {
               if (inscount)
                  INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR(cacheOnlyUpdateInsCount), IARG_THREAD_ID, IARG_UINT32, inscount, IARG_END);
               break;
            }
         }
      }
   }
}

void extraeImgCallback(IMG img, void *args)
{
   using namespace std;
   string img_name = IMG_Name(img);

   if (!extrae_image.linked)
   {
      extrae_image.linked = img_name.find("libmpitrace") != string::npos;
      if (extrae_image.linked)
      {
         extrae_image.top_addr = IMG_LowAddress(img);
         extrae_image.bottom_addr = IMG_HighAddress(img);

         if (KnobVerbose.Value())
            cerr << "[SIFT_RECORDER:" << app_id << "] Extrae has been detected."
                 << "[0x" << hex << extrae_image.top_addr << ", 0x" << hex << extrae_image.bottom_addr << "]" << endl;
      }
   }
}

void initRecorderBase()
{
#ifdef DEBUG
   std::cout << "initRecorderBase()" << std::endl;
   std::cout << "Shall we track VMAs? " << KnobTrackVMAs.Value() << std::endl;
#endif
   // Open the output file to write the VMAs
   if (KnobTrackVMAs.Value()){
      vma_output = new std::ofstream(KnobOutputFile.Value().c_str() + std::string(".vma"));

      if (!vma_output->is_open())
      {
         std::cerr << "Error: Could not open " << KnobOutputFile.Value() << ".vma for writing." << std::endl;
         exit(1);
      }
      else
      {
#ifdef DEBUG
         std::cout << "Opened " << KnobOutputFile.Value() << ".vma for writing." << std::endl;
#endif
      }
   }
   // -- //

   TRACE_AddInstrumentFunction(traceCallback, 0);

   extrae_image.linked = false;

   if (KnobExtraePreLoaded.Value() != 0)
   {
      IMG_AddInstrumentFunction(extraeImgCallback, 0);
   }
}
