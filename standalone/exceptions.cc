#include "exceptions.h"
#include "simulator.h"
#include "trace_manager.h"

#include <signal.h>
#include <stdio.h>
#include <execinfo.h>
#include <string.h>

static void exceptionHandler(int sig, siginfo_t *scp, void *ctxt)
{
   static bool in_handler = false;

   fprintf(stderr, "\n[SNIPER] Internal exception: %s. Access Address = %p\n\n", strsignal(sig), scp->si_addr);

   if (in_handler)
   {
      // Avoid recursion when the code below generates a new exception
      return;
   }
   else
      in_handler = true;

   // Hide errors caused by failing SIFT writers, the root cause is the timing model failure
   if (Sim()->getTraceManager())
   {
      Sim()->getTraceManager()->mark_done();
   }

   const int BACKTRACE_SIZE = 16;
   void * backtrace_buffer[BACKTRACE_SIZE];
   unsigned int backtrace_n = backtrace(backtrace_buffer, BACKTRACE_SIZE);

   FILE* fp = fopen(Sim()->getConfig()->formatOutputFileName("debug_backtrace.out").c_str(), "w");
   // Usually rdtsc address, use 0 to tell addr2line we're in standalone mode
   fprintf(fp, "sniper\n");
   fprintf(fp, "%" PRIdPTR "\n", (intptr_t)0);
   // Skip functions 0 (this is us) and 1 (a libc internal function)
   for(unsigned int i = 2; i < backtrace_n; ++i)
   {
      fprintf(fp, " %" PRIdPTR "", (intptr_t)backtrace_buffer[i]);
   }
   fprintf(fp, "\n");
   fprintf(fp, "%s. Access Address = %p\n\n", strsignal(sig), scp->si_addr);
   fclose(fp);

   // Disable the handler and re-raise the signal
   signal(sig, SIG_DFL);
   raise(sig);
}

void registerExceptionHandler()
{
   struct sigaction sa;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = SA_SIGINFO;
   sa.sa_sigaction = exceptionHandler;

   sigaction(SIGFPE, &sa, NULL);
   sigaction(SIGILL, &sa, NULL);
   sigaction(SIGPIPE, &sa, NULL);
   sigaction(SIGSEGV, &sa, NULL);
}
