#pragma once

#include "fixed_types.h"

#include "pin.H"
#include <pthread.h>

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
IntPtr emuClockGettime(clockid_t clk_id, struct timespec *tp);
IntPtr emuGettimeofday(struct timeval *tv, struct timezone *tz);
void emuKmpReapMonitor(THREADID threadIndex, CONTEXT *ctxt);

AFUNPTR getFunptr(CONTEXT* context, string func_name);

}
