#include "trace_rtn.h"
#include "globals.h"
#include "threads.h"

static void routineEnter(THREADID threadid, ADDRINT eip, ADDRINT esp)
{
   if ((any_thread_in_detail || KnobRoutineTracingOutsideDetailed.Value()) && thread_data[threadid].output)
   {
      thread_data[threadid].output->RoutineChange(Sift::RoutineEnter, eip, esp, thread_data[threadid].last_call_site);
      thread_data[threadid].last_routine = eip;
      thread_data[threadid].last_call_site = 0;
   }
}

static void routineExit(THREADID threadid, ADDRINT eip, ADDRINT esp)
{
   if ((any_thread_in_detail || KnobRoutineTracingOutsideDetailed.Value()) && thread_data[threadid].output)
   {
      thread_data[threadid].output->RoutineChange(Sift::RoutineExit, eip, esp);
      thread_data[threadid].last_routine = -1;
   }
}

static void routineAssert(THREADID threadid, ADDRINT eip, ADDRINT esp)
{
   if ((any_thread_in_detail || KnobRoutineTracingOutsideDetailed.Value())
       && thread_data[threadid].output && thread_data[threadid].last_routine != eip)
   {
      thread_data[threadid].output->RoutineChange(Sift::RoutineAssert, eip, esp);
      thread_data[threadid].last_routine = eip;
   }
}

static void routineCallSite(THREADID threadid, ADDRINT eip)
{
   thread_data[threadid].last_call_site = eip;
}

static void announceRoutine(INS ins)
{
   if (!thread_data[PIN_ThreadId()].output)
      return;

   ADDRINT eip = INS_Address(ins);
   RTN rtn = INS_Rtn(ins);
   IMG img = IMG_FindByAddress(eip);

   routines[eip] = true;

   INT32 column = 0, line = 0;
   std::string filename = "??";
   PIN_GetSourceLocation(eip, &column, &line, &filename);

   thread_data[PIN_ThreadId()].output->RoutineAnnounce(
      eip,
      RTN_Valid(rtn) ? RTN_Name(rtn).c_str() : "??",
      IMG_Valid(img) ? IMG_Name(img).c_str() : "??",
      IMG_Valid(img) ? IMG_LoadOffset(img) : 0,
      column, line, filename.c_str());
}

static void announceRoutine(RTN rtn)
{
   announceRoutine(RTN_InsHeadOnly(rtn));
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
      {
         RTN_Open(rtn);
         announceRoutine(rtn);
         RTN_Close(rtn);
      }

      TRACE_InsertCall(trace, IPOINT_BEFORE, AFUNPTR (routineAssert), IARG_THREAD_ID, IARG_ADDRINT, RTN_Address(rtn), IARG_REG_VALUE, REG_STACK_PTR, IARG_END);
   }
   else
   {
      if (routines.count(0) == 0)
         announceInvalidRoutine();

      TRACE_InsertCall(trace, IPOINT_BEFORE, AFUNPTR (routineAssert), IARG_THREAD_ID, IARG_ADDRINT, 0, IARG_REG_VALUE, REG_STACK_PTR, IARG_END);
   }

   // Call site identification
   for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
      for(INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins))
      {
         if (INS_IsProcedureCall(ins))
         {
            announceRoutine(ins);
            INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR (routineCallSite), IARG_THREAD_ID, IARG_INST_PTR, IARG_END);
         }
      }
}

void initRoutineTracer()
{
   RTN_AddInstrumentFunction(routineCallback, 0);
   TRACE_AddInstrumentFunction(traceCallback, 0);
}
