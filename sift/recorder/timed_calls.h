#ifndef __TIMED_CALLS_H
#define __TIMED_CALLS_H

#include "pin.H"
#include "globals.h"

/* The guest's timeouts in the guest's time (sift/guest_time.h): the timed
   waits the simulator does not model, and, without a simulator to answer,
   the one-IPC model's clocks.  timed_calls.cc. */

void timedInit();
void timedThreadStart(THREADID threadid, INT32 parent);
void timedThreadExit(THREADID threadid);

/* The thread's time in the one-IPC model (ns from the clocks' zero), its
   CPU time and the process's */
UINT64 timedNow(THREADID threadid);
UINT64 timedThreadCpu(THREADID threadid);
UINT64 timedProcessCpu();

/* A system call about to run: true when it was run here (a timed wait) and
   *ret is its result; otherwise the bookkeeping is done and it runs */
bool timedSyscall(THREADID threadid, ADDRINT num, const syscall_args_t &args, ADDRINT *ret);
/* A system call that ran returned */
void timedSyscallReturned(THREADID threadid, ADDRINT ret);

#endif // __TIMED_CALLS_H
