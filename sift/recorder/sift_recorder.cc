#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <syscall.h>
#include <vector>

#include <cstdio>
#include <cassert>
#include <unistd.h>
#include <sys/types.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <string.h>
#include <pthread.h>

#include "pin.H"

#include "globals.h"
#include "threads.h"
#include "recorder_control.h"
#include "recorder_base.h"
#include "syscall_modeling.h"
#include "trace_rtn.h"
#include "emulation.h"
#include "sift_writer.h"
#include "sift_assert.h"
#include "pinboost_debug.h"

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

   PIN_InitLock(&access_memory_lock);
   PIN_InitLock(&new_threadid_lock);

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
   else if (KnobEmulateSyscalls.Value())
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

      initSyscallModeling();
   }

   initThreads();
   initRecorderControl();
   initRecorderBase();
   initEmulation();

   if (KnobRoutineTracing.Value())
      initRoutineTracer();

   PIN_AddFiniFunction(Fini, 0);
   PIN_AddDetachFunction(Detach, 0);

   PIN_AddFollowChildProcessFunction(followChild, 0);
   PIN_AddForkFunction(FPOINT_BEFORE, forkBefore, 0);

   pinboost_register("SIFT_RECORDER", KnobDebug.Value());

   PIN_StartProgram();

   return 0;
}
