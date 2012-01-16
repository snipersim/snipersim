#pragma once

#include "fixed_types.h"
#include "thread_support.h"

#include "pin.H"
#include <pthread.h>

namespace lite
{

void routineCallback(RTN rtn, void* v);

carbon_thread_t emuCarbonSpawnThread(CONTEXT* context, thread_func_t thread_func, void* arg);
void emuPthreadCreateBefore(THREADID thread_id, ADDRINT thread_ptr, void* (*thread_func)(void*), void* arg);
void emuPthreadCreateAfter(THREADID thread_id);
void emuCarbonJoinThread(CONTEXT* context, carbon_thread_t tid);
void emuPthreadJoinBefore(pthread_t thread);
IntPtr nullFunction();

void pthreadBefore(THREADID thread_id);
void pthreadAfter(THREADID thread_id, ADDRINT type_id, ADDRINT retval);

// os emulation
IntPtr emuGetNprocs();

AFUNPTR getFunptr(CONTEXT* context, string func_name);

}
