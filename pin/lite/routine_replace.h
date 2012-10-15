#pragma once

#include "fixed_types.h"

#include "pin.H"
#include <pthread.h>
#include <sys/time.h>

namespace lite
{

void routineCallback(RTN rtn, void* v);
void routineStartCallback(RTN rtn, INS ins);

void emuPthreadCreateBefore(THREADID thread_id, ADDRINT thread_ptr, void* (*thread_func)(void*), void* arg);
void emuPthreadCreateAfter(THREADID thread_id);
void emuPthreadJoinBefore(THREADID thread_id, pthread_t thread);
IntPtr nullFunction();

void pthreadBefore(THREADID thread_id);
void pthreadAfter(THREADID thread_id, ADDRINT type_id, ADDRINT retval);

// os emulation
IntPtr emuGetNprocs();
IntPtr emuGetCPU(THREADID thread_id);
IntPtr emuClockGettime(THREADID thread_id, clockid_t clk_id, struct timespec *tp);
IntPtr emuGettimeofday(THREADID thread_id, struct timeval *tv, struct timezone *tz);
void emuKmpReapMonitor(THREADID threadIndex, CONTEXT *ctxt);

AFUNPTR getFunptr(CONTEXT* context, string func_name);

}
