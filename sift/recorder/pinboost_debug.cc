#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "pinboost_debug.h"
#include "callstack.h"

#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <iostream>
#include <cstdlib>

bool assert_ignore();

bool pinboost_do_debug = false;
static std::string pinboost_name = "PINBOOST";

static EXCEPT_HANDLING_RESULT pinboost_exceptionhandler(THREADID threadid, EXCEPTION_INFO *pExceptInfo, PHYSICAL_CONTEXT *pPhysCtxt, VOID *v)
{
   static bool in_handler = false;

   if (assert_ignore())
   {
      // Timing model says it's done, ignore assert and pretend to have exited cleanly
      exit(0);
   }

   if (!in_handler)
   {
      // Avoid recursion when the code below generates a new exception
      in_handler = true;

      bool success = pinboost_backtrace(pExceptInfo, pPhysCtxt);
      // Light error message in case printing a full backtrace failed
      if (!success)
         std::cerr << "["<<pinboost_name<<"] Internal exception:" << PIN_ExceptionToString(pExceptInfo) << std::endl;

      if (pinboost_do_debug)
         pinboost_debugme(threadid);
   }
   else
   {
      std::cerr << "["<<pinboost_name<<"] Internal exception:" << PIN_ExceptionToString(pExceptInfo) << std::endl;

      // Error occurred while handling another error (either in the exception handler, or in another thread)
      // Debug session should already be started, let's ignore this error and halt so the user can inspect the state.
      pause();
   }

   return EHR_CONTINUE_SEARCH;
}

void pinboost_register(const char* name, bool do_screen_debug)
{
   PIN_AddInternalExceptionHandler(pinboost_exceptionhandler, NULL);
   if (name)
      pinboost_name = name;
   if (do_screen_debug)
      pinboost_do_debug = true;
}

void rdtsc() {}

bool pinboost_backtrace(EXCEPTION_INFO *pExceptInfo, PHYSICAL_CONTEXT *pPhysCtxt)
{
   const int BACKTRACE_SIZE = 16;
   void * backtrace_buffer[BACKTRACE_SIZE];
   unsigned int backtrace_n = get_call_stack_from(backtrace_buffer, BACKTRACE_SIZE,
                                                  (void*)PIN_GetPhysicalContextReg(pPhysCtxt, LEVEL_BASE::REG_STACK_PTR),
                                                  (void*)PIN_GetPhysicalContextReg(pPhysCtxt, LEVEL_BASE::REG_GBP)
   );

   char backtrace_filename[1024];
   sprintf(backtrace_filename, "/tmp/debug_backtrace_%ld.out", syscall(__NR_gettid));

   FILE* fp = fopen(backtrace_filename, "w");
   // so addr2line can calculate the offset where we're really mapped
   fprintf(fp, "sift_recorder\n");
   fprintf(fp, "%" PRIdPTR "\n", (intptr_t)rdtsc);
   // actual function function where the exception occured (won't be in the backtrace)
   fprintf(fp, "%" PRIdPTR "", (intptr_t)PIN_GetPhysicalContextReg(pPhysCtxt, LEVEL_BASE::REG_INST_PTR));
   for(unsigned int i = 0; i < backtrace_n; ++i)
   {
      fprintf(fp, " %" PRIdPTR "", (intptr_t)backtrace_buffer[i]);
   }
   fprintf(fp, "\n");
   fprintf(fp, "%s\n", PIN_ExceptionToString(pExceptInfo).c_str());
   fclose(fp);

   if (getenv("SNIPER_ROOT") == NULL)
   {
      std::cerr << "[" << pinboost_name << "] SNIPER_ROOT not set, cannot launch debugger." << std::endl;
      return false;
   }

   char cmd[1024];
   sprintf(cmd, "%s/tools/gen_backtrace.py \"%s\" >&2", getenv("SNIPER_ROOT"), backtrace_filename);

   int rc = system(cmd);
   if (rc)
   {
      std::cerr << "[" << pinboost_name << "] Failed to print backtrace." << std::endl;
      return false;
   }

   return true;
}

void pinboost_debugme(THREADID threadid)
{
   if (getenv("SNIPER_ROOT") == NULL)
   {
      std::cerr << "[" << pinboost_name << "] SNIPER_ROOT not set, cannot launch debugger." << std::endl;
      return;
   }

   // If we are the faulting thread, pass in our tid so the debug script can mark us
   pid_t tid = PIN_ThreadId() == threadid ? PIN_GetTid() : 0;

   char cmd[1024];
   sprintf(cmd, "%s/tools/pinboost_debugme.py %d %d", getenv("SNIPER_ROOT"), getpid(), tid);

   int rc = system(cmd);
   if (rc)
   {
      std::cerr << "[" << pinboost_name << "] Failed to start debugging session." << std::endl;
      return;
   }

   // Give the debugger some time to attach, after that, fall through
   sleep(5);
}

void pinboost_assert_fail(const char *__assertion, const char *__file, unsigned int __line, const char *__function)
{
   std::cerr << "[" << pinboost_name << "] " << __file << ":" << __line << ": " <<__function
             << ": Assertion `" << __assertion << "' failed." << std::endl;
   if (pinboost_do_debug)
      pinboost_debugme(PIN_ThreadId());
   exit(0);
}
