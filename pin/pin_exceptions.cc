#include "pin_exceptions.h"
#include "simulator.h"
#include "config.h"
#include "timer.h"
#include "callstack.h"

EXCEPT_HANDLING_RESULT exceptionHandler(THREADID tid, EXCEPTION_INFO *pExceptInfo, PHYSICAL_CONTEXT *pPhysCtxt, VOID *v)
{
   static bool in_handler = false;

   fprintf(stderr, "[SNIPER] Internal exception: %s\n", PIN_ExceptionToString(pExceptInfo).c_str());

   if (in_handler)
   {
      // Avoid recursion when the code below generates a new exception
      return EHR_UNHANDLED;
   }
   else
      in_handler = true;

   const int BACKTRACE_SIZE = 16;
   void * backtrace_buffer[BACKTRACE_SIZE];
   unsigned int backtrace_n = get_call_stack_from(backtrace_buffer, BACKTRACE_SIZE,
                                                  (void*)PIN_GetPhysicalContextReg(pPhysCtxt, LEVEL_BASE::REG_STACK_PTR),
                                                  (void*)PIN_GetPhysicalContextReg(pPhysCtxt, LEVEL_BASE::REG_GBP)
   );

   FILE* fp = fopen(Sim()->getConfig()->formatOutputFileName("debug_backtrace.out").c_str(), "w");
   // so addr2line can calculate the offset where we're really mapped
   fprintf(fp, "pin_sim.so\n");
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

   return EHR_CONTINUE_SEARCH;
}
