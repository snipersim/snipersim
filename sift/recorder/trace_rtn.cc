#include "trace_rtn.h"
#include "globals.h"
#include "threads.h"

static void routineEnter(THREADID threadid, ADDRINT eip, ADDRINT esp)
{
   if ((any_thread_in_detail || KnobRoutineTracingOutsideDetailed.Value()) && thread_data[threadid].output)
   {
      thread_data[threadid].output->RoutineChange(eip, esp, Sift::RoutineEnter);
      thread_data[threadid].last_routine = eip;
   }
}

static void routineExit(THREADID threadid, ADDRINT eip, ADDRINT esp)
{
   if ((any_thread_in_detail || KnobRoutineTracingOutsideDetailed.Value()) && thread_data[threadid].output)
   {
      thread_data[threadid].output->RoutineChange(eip, esp, Sift::RoutineExit);
      thread_data[threadid].last_routine = -1;
   }
}

static void routineAssert(THREADID threadid, ADDRINT eip, ADDRINT esp)
{
   if ((any_thread_in_detail || KnobRoutineTracingOutsideDetailed.Value())
       && thread_data[threadid].output && thread_data[threadid].last_routine != eip)
   {
      thread_data[threadid].output->RoutineChange(eip, esp, Sift::RoutineAssert);
      thread_data[threadid].last_routine = eip;
   }
}

static void announceRoutine(RTN rtn)
{
   if (!thread_data[PIN_ThreadId()].output)
      return;

   routines[RTN_Address(rtn)] = true;

   INT32 column = 0, line = 0;
   std::string filename = "??";
   PIN_GetSourceLocation(RTN_Address(rtn), &column, &line, &filename);
   IMG img = IMG_FindByAddress(RTN_Address(rtn));
   thread_data[PIN_ThreadId()].output->RoutineAnnounce(
      RTN_Address(rtn),
      RTN_Name(rtn).c_str(),
      IMG_Valid(img) ? IMG_Name(img).c_str() : "??",
      IMG_Valid(img) ? IMG_LoadOffset(img) : 0,
      column, line, filename.c_str());
}

static void announceInvalidRoutine()
{
   if (!thread_data[PIN_ThreadId()].output)
      return;

   routines[0] = true;
   thread_data[PIN_ThreadId()].output->RoutineAnnounce(0, "INVALID", "", 0, 0, 0, "");
}

static void routineCallback(RTN rtn, VOID *v)
{
   RTN_Open(rtn);

   if (routines.count(RTN_Address(rtn)) == 0)
      announceRoutine(rtn);

   RTN_InsertCall(rtn, IPOINT_BEFORE, AFUNPTR(routineEnter), IARG_THREAD_ID, IARG_ADDRINT, RTN_Address(rtn), IARG_REG_VALUE, REG_STACK_PTR, IARG_END);
   RTN_InsertCall(rtn, IPOINT_AFTER,  AFUNPTR(routineExit), IARG_THREAD_ID, IARG_ADDRINT, RTN_Address(rtn), IARG_REG_VALUE, REG_STACK_PTR, IARG_END);

   RTN_Close(rtn);
}

static void traceCallback(TRACE trace, VOID *v)
{
   // At the start of each trace, check to see if this part of the code belongs to the function we think we're in.
   // This will detect longjmps and tail call elimination, and fix up the call stack appropriately.
   RTN rtn = TRACE_Rtn(trace);

   if (RTN_Valid(rtn))
   {
      if (routines.count(RTN_Address(rtn)) == 0)
         announceRoutine(rtn);

      TRACE_InsertCall(trace, IPOINT_BEFORE, AFUNPTR (routineAssert), IARG_THREAD_ID, IARG_ADDRINT, RTN_Address(rtn), IARG_REG_VALUE, REG_STACK_PTR, IARG_END);
   }
   else
   {
      if (routines.count(0) == 0)
         announceInvalidRoutine();

      TRACE_InsertCall(trace, IPOINT_BEFORE, AFUNPTR (routineAssert), IARG_THREAD_ID, IARG_ADDRINT, 0, IARG_REG_VALUE, REG_STACK_PTR, IARG_END);
   }
}

void initRoutineTracer()
{
   RTN_AddInstrumentFunction(routineCallback, 0);
   TRACE_AddInstrumentFunction(traceCallback, 0);
}
