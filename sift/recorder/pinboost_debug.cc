#include "pinboost_debug.h"

#include <sys/types.h>
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

   std::cerr << "["<<pinboost_name<<"] Internal exception:" << PIN_ExceptionToString(pExceptInfo) << std::endl;

   if (!in_handler)
   {
      // Avoid recursion when the code below generates a new exception
      in_handler = true;

      pinboost_debugme(threadid);
   }
   else
   {
      // Error occurred while handling another error (either in the exception handler, or in another thread)
      // Debug session should already be started, let's ignore this error and halt so the user can inspect the state.
      pause();
   }

   return EHR_CONTINUE_SEARCH;
}


void pinboost_register(const char* name)
{
   PIN_AddInternalExceptionHandler(pinboost_exceptionhandler, NULL);
   if (name)
      pinboost_name = name;
   pinboost_do_debug = true;
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
