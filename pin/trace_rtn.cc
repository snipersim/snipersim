#include "trace_rtn.h"
#include "simulator.h"
#include "local_storage.h"
#include "routine_tracer.h"

void routineEnter(THREADID threadIndex, IntPtr eip, IntPtr esp)
{
   localStore[threadIndex].thread->getRoutineTracer()->routineEnter(eip, esp);
}

void routineExit(THREADID threadIndex, IntPtr eip, IntPtr esp)
{
   localStore[threadIndex].thread->getRoutineTracer()->routineExit(eip, esp);
}

void routineAssert(THREADID threadIndex, IntPtr eip, IntPtr esp)
{
   localStore[threadIndex].thread->getRoutineTracer()->routineAssert(eip, esp);
}

void announceRoutine(RTN rtn)
{
   INT32 column = 0, line = 0;
   std::string filename = "??";
   PIN_GetSourceLocation(RTN_Address(rtn), &column, &line, &filename);
   IMG img = IMG_FindByAddress(RTN_Address(rtn));
   Sim()->getRoutineTracer()->addRoutine(
      RTN_Address(rtn),
      RTN_Name(rtn).c_str(),
      IMG_Valid(img) ? IMG_Name(img).c_str() : "??",
      IMG_Valid(img) ? IMG_LoadOffset(img) : 0,
      column, line, filename.c_str());
}

void announceInvalidRoutine()
{
   Sim()->getRoutineTracer()->addRoutine(0, "INVALID", "", 0, 0, 0, "");
}

void addRtnTracer(RTN rtn)
{
   if (Sim()->getRoutineTracer())
   {
      RTN_Open(rtn);

      announceRoutine(rtn);

      RTN_InsertCall(rtn, IPOINT_BEFORE, AFUNPTR(routineEnter), IARG_THREAD_ID, IARG_ADDRINT, RTN_Address(rtn), IARG_REG_VALUE, REG_STACK_PTR, IARG_END);
      RTN_InsertCall(rtn, IPOINT_AFTER,  AFUNPTR(routineExit), IARG_THREAD_ID, IARG_ADDRINT, RTN_Address(rtn), IARG_REG_VALUE, REG_STACK_PTR, IARG_END);

      RTN_Close(rtn);
   }
}

void addRtnTracer(TRACE trace)
{
   if (Sim()->getRoutineTracer())
   {
      // At the start of each trace, check to see if this part of the code belongs to the function we think we're in.
      // This will detect longjmps and tail call elimination, and fix up the call stack appropriately.
      RTN rtn = TRACE_Rtn(trace);

      if (RTN_Valid(rtn))
      {
         if (!Sim()->getRoutineTracer()->hasRoutine(RTN_Address(rtn)))
            announceRoutine(rtn);

         TRACE_InsertCall(trace, IPOINT_BEFORE, AFUNPTR (routineAssert), IARG_THREAD_ID, IARG_ADDRINT, RTN_Address(rtn), IARG_REG_VALUE, REG_STACK_PTR, IARG_END);
      }
      else
      {
         if (!Sim()->getRoutineTracer()->hasRoutine(0))
            announceInvalidRoutine();

         TRACE_InsertCall(trace, IPOINT_BEFORE, AFUNPTR (routineAssert), IARG_THREAD_ID, IARG_ADDRINT, 0, IARG_REG_VALUE, REG_STACK_PTR, IARG_END);
      }
   }
}
