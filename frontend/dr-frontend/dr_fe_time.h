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
 * x86-64 only: on another ISA (or with -refuse_time, which tests it on
 * x86-64) the guest's time is not kept, and its first use of time -- a
 * clock read, a wait with a timeout, a timer -- ends the run with the
 * reason in <output>.app<N>.refused, rather than the host's time reaching
 * the guest. */

#include <stdint.h>
#include "frontend_defs.h"

namespace drtime
{
   /* td: the front end's thread data; emulate: -e (a simulator answers);
      time_start: the guest's time zero, seconds since the epoch;
      refuse_time: refuse the guest's use of time, as on another ISA;
      output and app_id name the refusal's file */
   void init(thread_data_t *td, bool emulate, uint64_t time_start, bool refuse_time, const char *output, int app_id);
   void exit();
   /* false where the guest's time is not kept (another ISA, -refuse_time) */
   bool supported();
   /* the guest's time is not kept: its first use ends the run */
   bool refusing();
   /* end the run: the guest used time (what) where it is not kept */
   void refuse(const char *what);
   /* where the guest's time is not kept, a counter read's value: the
      instructions run (at 1 GHz), never less than another thread read */
   const uint64_t COUNTER_HZ = 1000000000;
   uint64_t counter_ticks(threadid_t threadid);

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
