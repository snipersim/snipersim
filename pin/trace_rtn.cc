#include "trace_rtn.h"
#include "simulator.h"
#include "local_storage.h"
#include "routine_tracer.h"

void routineEnter(THREADID threadIndex, IntPtr eip, IntPtr esp)
{
   localStore[threadIndex].thread->getRoutineTracer()->routineEnter(eip, esp, localStore[threadIndex].lastCallSite);
   localStore[threadIndex].lastCallSite = 0;
}

void routineExit(THREADID threadIndex, IntPtr eip, IntPtr esp)
{
   localStore[threadIndex].thread->getRoutineTracer()->routineExit(eip, esp);
}

void routineAssert(THREADID threadIndex, IntPtr eip, IntPtr esp)
{
   localStore[threadIndex].thread->getRoutineTracer()->routineAssert(eip, esp);
}

void routineCallSite(THREADID threadIndex, IntPtr eip)
{
   localStore[threadIndex].lastCallSite = eip;
}

void announceRoutine(INS ins)
{
   ADDRINT eip = INS_Address(ins);
   RTN rtn = INS_Rtn(ins);
   IMG img = IMG_FindByAddress(eip);

   INT32 column = 0, line = 0;
   std::string filename = "??";
   PIN_GetSourceLocation(eip, &column, &line, &filename);

   Sim()->getRoutineTracer()->addRoutine(
      eip,
      RTN_Valid(rtn) ? RTN_Name(rtn).c_str() : "??",
      IMG_Valid(img) ? IMG_Name(img).c_str() : "??",
      IMG_Valid(img) ? IMG_LoadOffset(img) : 0,
      column, line, filename.c_str());
}

void announceRoutine(RTN rtn)
{
   announceRoutine(RTN_InsHeadOnly(rtn));
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
}
