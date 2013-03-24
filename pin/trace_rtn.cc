#include "trace_rtn.h"
#include "simulator.h"
#include "local_storage.h"
#include "routine_tracer.h"

void routineEnter(THREADID threadIndex, IntPtr eip)
{
   localStore[threadIndex].thread->getRoutineTracer()->routineEnter(eip);
}

void routineExit(THREADID threadIndex, IntPtr eip)
{
   localStore[threadIndex].thread->getRoutineTracer()->routineExit(eip);
}

void routineAssert(THREADID threadIndex, IntPtr eip)
{
   localStore[threadIndex].thread->getRoutineTracer()->routineAssert(eip);
}

void addRtnTracer(RTN rtn)
{
   if (Sim()->getRoutineTracer())
   {
      RTN_Open (rtn);

      RTN_InsertCall (rtn, IPOINT_BEFORE, AFUNPTR (routineEnter), IARG_THREAD_ID, IARG_ADDRINT, RTN_Address(rtn), IARG_END);
      RTN_InsertCall (rtn, IPOINT_AFTER,  AFUNPTR (routineExit), IARG_THREAD_ID, IARG_ADDRINT, RTN_Address(rtn), IARG_END);

      INT32 column = 0, line = 0;
      std::string filename = "??";
      PIN_GetSourceLocation(RTN_Address(rtn), &column, &line, &filename);
      Sim()->getRoutineTracer()->addRoutine(RTN_Address(rtn), RTN_Name(rtn).c_str(), column, line, filename.c_str());

      RTN_Close (rtn);
   }
}

void addRtnTracer(TRACE trace)
{
   if (Sim()->getRoutineTracer())
   {
      // At the start of each trace, check to see if this part of the code belongs to the function we think we're in.
      // This will detect longjmps and tail call elimination, and fix up the call stack appropriately.
      RTN rtn = TRACE_Rtn(trace);
      TRACE_InsertCall(trace, IPOINT_BEFORE, AFUNPTR (routineAssert), IARG_THREAD_ID, IARG_ADDRINT, RTN_Address(rtn), IARG_END);
   }
}
