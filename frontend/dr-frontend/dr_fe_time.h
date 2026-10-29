#ifndef _DR_FE_TIME_H_
#define _DR_FE_TIME_H_

/* The guest's time under the DynamoRIO front end (sift/guest_time.h):
 * no time the guest gives reaches the host's kernel, and the guest reads
 * simulated time, never the host's.  dr_fe_time.cc, the port of the SIFT
 * recorder's timed_calls.cc and of its clock answers (emulation.cc,
 * syscall_modeling.cc).
 *
 * Live (a simulator answering: -e with the thread's channel open) the
 * clocks are the simulator's, futex and the sleeps are its own (doSyscall
 * forwards them), every other timed wait runs as probes between which the
 * simulator lets the thread sleep in simulated time; otherwise (a trace
 * written to a file) the one-IPC model's clocks, from the time zero
 * (-time_start, osemu/time_start), and the timed waits end at their
 * deadlines in that time.  The timers (alarm, setitimer, the POSIX timers,
 * timerfd) are kept here and fired in the guest's time.
 *
 * x86-64 only: on another ISA init() says so and every call here is a
 * no-op (the guest then reads the host's clocks and its timeouts are the
 * host kernel's). */

#include <stdint.h>
#include "frontend_defs.h"

namespace drtime
{
   /* td: the front end's thread data; emulate: -e (a simulator answers);
      time_start: the guest's time zero, seconds since the epoch */
   void init(thread_data_t *td, bool emulate, uint64_t time_start);
   void exit();
   /* false on an ISA this module does not handle */
   bool supported();

   /* A thread starts (its stack pointer at its first instruction, 0 if
      unknown, pairs it with its clone and so with its parent) and ends */
   void thread_start(threadid_t threadid, uint64_t start_sp);
   void thread_exit(threadid_t threadid);

   /* Instructions the thread ran (sent to the trace or counted) */
   void count(threadid_t threadid, uint64_t instructions);
   /* At a block: the timers, live, every so many instructions.  True when
      a signal for the thread waits in DynamoRIO, which delivers it as the
      thread leaves the code cache: a timer fired here, or the client
      thread signalled it */
   bool flow(threadid_t threadid);
   /* DynamoRIO delivers a signal to the thread */
   void signal_delivered(threadid_t threadid);

   /* A system call about to run: true when it was answered here (*ret its
      result, the call skipped); otherwise the bookkeeping is done and it
      runs.  args as the guest passes them. */
   bool syscall(threadid_t threadid, int num, const uint64_t args[6], uint64_t *ret);
   /* A system call that ran returned */
   void syscall_returned(threadid_t threadid, int64_t ret);

   /* The guest's clocks: clock_gettime's clock (ns since the epoch for the
      time clocks, from zero for the CPU-time ones); false for a clock that
      does not exist (EINVAL) */
   bool clock_ns(threadid_t threadid, int64_t clock, uint64_t *ns);
   /* rdtsc */
   uint64_t tsc(threadid_t threadid);
}

#endif // _DR_FE_TIME_H_
