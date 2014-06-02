#include "recorder_control.h"
#include "globals.h"
#include "threads.h"
#include "syscall_modeling.h"
#include "sift_assert.h"
#include "../../include/sim_api.h"

#include <iostream>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>

void beginROI(THREADID threadid)
{
   if (app_id < 0)
      findMyAppId();

   if (in_roi)
   {
      std::cerr << "[SIFT_RECORDER:" << app_id << "] Error: ROI_START seen, but we have already started." << std::endl;
   }
   else
   {
      if (KnobVerbose.Value())
         std::cerr << "[SIFT_RECORDER:" << app_id << "] ROI Begin" << std::endl;
   }

   in_roi = true;
   setInstrumentationMode(Sift::ModeDetailed);

   if (KnobEmulateSyscalls.Value())
   {
      if (thread_data[threadid].icount_reported > 0)
      {
         thread_data[threadid].output->InstructionCount(thread_data[threadid].icount_reported);
         thread_data[threadid].icount_reported = 0;
      }
   }
   else
   {
      for (unsigned int i = 0 ; i < MAX_NUM_THREADS ; i++)
      {
         if (thread_data[i].running && !thread_data[i].output)
            openFile(i);
      }
   }
}

void endROI(THREADID threadid)
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

   if (KnobVerbose.Value())
      std::cerr << "[SIFT_RECORDER:" << app_id << "] ROI End" << std::endl;

   // Stop threads from sending any more data while we close the SIFT pipes
   setInstrumentationMode(Sift::ModeIcount);
   in_roi = false;

   if (!KnobUseResponseFiles.Value())
   {
      for (unsigned int i = 0 ; i < MAX_NUM_THREADS ; i++)
      {
         if (thread_data[i].running && thread_data[i].output)
            closeFile(i);
      }
   }
}

void setInstrumentationMode(Sift::Mode mode)
{
   if (current_mode != mode && mode != Sift::ModeUnknown)
   {
      current_mode = mode;
      switch(mode)
      {
         case Sift::ModeIcount:
            any_thread_in_detail = false;
            break;
         case Sift::ModeMemory:
         case Sift::ModeDetailed:
            any_thread_in_detail = true;
            break;
         case Sift::ModeStop:
            for (unsigned int i = 0 ; i < MAX_NUM_THREADS ; i++)
            {
               if (thread_data[i].output)
                  closeFile(i);
            }
            exit(0);
         case Sift::ModeUnknown:
            assert(false);
      }
      PIN_RemoveInstrumentation();
   }
}

ADDRINT handleMagic(THREADID threadid, ADDRINT gax, ADDRINT gbx, ADDRINT gcx)
{
   uint64_t res = gax; // Default: don't modify gax

   if (KnobUseResponseFiles.Value() && thread_data[threadid].running && thread_data[threadid].output)
   {
      res = thread_data[threadid].output->Magic(gax, gbx, gcx);
   }

   if (gax == SIM_CMD_ROI_START)
   {
      if (KnobUseROI.Value() && !in_roi)
         beginROI(threadid);
   }
   else if (gax == SIM_CMD_ROI_END)
   {
      if (KnobUseROI.Value() && in_roi)
         endROI(threadid);
   }

   return res;
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

VOID handleRoutineImplicitROI(THREADID threadid, bool begin)
{
   if (begin)
   {
      thread_data[threadid].output->Magic(SIM_CMD_ROI_START, 0, 0);
      beginROI(threadid);
   }
   else
   {
      thread_data[threadid].output->Magic(SIM_CMD_ROI_END, 0, 0);
      endROI(threadid);
   }
}

static void routineCallback(RTN rtn, void* v)
{
   if (KnobMPIImplicitROI.Value())
   {
      std::string rtn_name = RTN_Name(rtn);
      if (rtn_name.find("MPI_Init") != string::npos && rtn_name.find("MPI_Initialized") == string::npos) // Actual name can be MPI_Init, MPI_Init_thread, PMPI_Init_thread, etc.
      {
         RTN_Open(rtn);
         RTN_InsertCall(rtn, IPOINT_AFTER, AFUNPTR(handleRoutineImplicitROI), IARG_THREAD_ID, IARG_BOOL, true, IARG_END);
         RTN_Close(rtn);
      }
      if (rtn_name.find("MPI_Finalize") != string::npos)
      {
         RTN_Open(rtn);
         RTN_InsertCall(rtn, IPOINT_BEFORE, AFUNPTR(handleRoutineImplicitROI), IARG_THREAD_ID, IARG_BOOL, false, IARG_END);
         RTN_Close(rtn);
      }
   }
}

// PinPlay w/ address translation: Translate IARG_MEMORYOP_EA to actual address
ADDRINT translateAddress(ADDRINT addr, ADDRINT size)
{
   MEMORY_ADDR_TRANS_CALLBACK memory_trans_callback = 0;

   // Get memory translation callback if exists
   memory_trans_callback = PIN_GetMemoryAddressTransFunction();

   // Check if we have a callback
   if (memory_trans_callback != 0)
   {
       // Prepare callback structure
       PIN_MEM_TRANS_INFO mem_trans_info;
       mem_trans_info.addr = addr;
       mem_trans_info.bytes = size;
       mem_trans_info.ip = 0; //ip;
       mem_trans_info.memOpType = PIN_MEMOP_LOAD;
       mem_trans_info.threadIndex = 0; //threadid;

       // Get translated address from callback directly
       addr = memory_trans_callback(&mem_trans_info, 0);
   }

   return addr;
}

static void getCode(uint8_t *dst, const uint8_t *src, uint32_t size)
{
   PIN_SafeCopy(dst, (void*)translateAddress(ADDRINT(src), size), size);
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

   if (KnobVerbose.Value())
      std::cerr << "[SIFT_RECORDER:" << app_id << ":" << thread_data[threadid].thread_num << "] Output = [" << filename << "]" << std::endl;

   if (KnobUseResponseFiles.Value())
   {
      sprintf(response_filename, "%s_response.app%" PRId32 ".th%" PRIu64 ".sift", KnobOutputFile.Value().c_str(), app_id, thread_data[threadid].thread_num);
      if (KnobVerbose.Value())
         std::cerr << "[SIFT_RECORDER:" << app_id << ":" << thread_data[threadid].thread_num << "] Response = [" << response_filename << "]" << std::endl;
   }


   // Open the file for writing
   try {
      #ifdef TARGET_IA32
         const bool arch32 = true;
      #else
         const bool arch32 = false;
      #endif
      thread_data[threadid].output = new Sift::Writer(filename, getCode, KnobUseResponseFiles.Value() ? false : true, response_filename, threadid, arch32, false, KnobSendPhysicalAddresses.Value());
   } catch (...) {
      std::cerr << "[SIFT_RECORDER:" << app_id << ":" << thread_data[threadid].thread_num << "] Error: Unable to open the output file " << filename << std::endl;
      exit(1);
   }

   thread_data[threadid].output->setHandleAccessMemoryFunc(handleAccessMemory, reinterpret_cast<void*>(threadid));
}

void closeFile(THREADID threadid)
{
   if (KnobVerbose.Value())
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

void initRecorderControl()
{
   RTN_AddInstrumentFunction(routineCallback, 0);
}
