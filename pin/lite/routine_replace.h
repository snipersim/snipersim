#pragma once

#include "fixed_types.h"

#include "pin.H"
#include <pthread.h>
#include <sys/time.h>

namespace lite
{

void routineCallback(RTN rtn, void* v);
void routineStartCallback(RTN rtn, INS ins);

IntPtr nullFunction();

void pthreadBefore(THREADID thread_id);
void pthreadAfter(THREADID thread_id, ADDRINT type_id, ADDRINT retval);

void mallocBefore(THREADID thread_id, ADDRINT eip, ADDRINT size);
void mallocAfter(THREADID thread_id, ADDRINT address);
void freeBefore(THREADID thread_id, ADDRINT eip, ADDRINT address);

// os emulation
IntPtr emuGetNprocs();
IntPtr emuGetCPU(THREADID thread_id);
IntPtr emuClockGettime(THREADID thread_id, clockid_t clk_id, struct timespec *tp);
IntPtr emuGettimeofday(THREADID thread_id, struct timeval *tv, struct timezone *tz);
void emuKmpReapMonitor(THREADID threadIndex, CONTEXT *ctxt);

AFUNPTR getFunptr(CONTEXT* context, string func_name);

}
