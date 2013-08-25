#ifndef SYSCALL_SERVER_H
#define SYSCALL_SERVER_H

#include "fixed_types.h"
#include "subsecond_time.h"

#include <iostream>
#include <unordered_map>
#include <list>

// -- For futexes --
#include <linux/futex.h>
#include <sys/time.h>
#include <errno.h>

class Core;

// -- Special Class to Handle Futexes
class SimFutex
{
   public:
      struct Waiter
      {
         Waiter(thread_id_t _thread_id, int _mask, SubsecondTime _timeout)
            : thread_id(_thread_id), mask(_mask), timeout(_timeout)
            {}
         thread_id_t thread_id;
         int mask;
         SubsecondTime timeout;
      };
      typedef std::list<Waiter> ThreadQueue;

   private:
      ThreadQueue m_waiting;

   public:
      SimFutex();
      ~SimFutex();
      bool enqueueWaiter(thread_id_t thread_id, int mask, SubsecondTime time, SubsecondTime timeout_time, SubsecondTime &time_end);
      thread_id_t dequeueWaiter(thread_id_t thread_by, int mask, SubsecondTime time);
      thread_id_t requeueWaiter(SimFutex *requeue_futex);
      void wakeTimedOut(SubsecondTime time);
      SubsecondTime getNextTimeout(SubsecondTime time);
};

class SyscallServer
{
   public:
      struct futex_args_t {
         int *uaddr;
         int op;
         int val;
         const struct timespec *timeout;
         int val2;
         int *uaddr2;
         int val3;
      };

      SyscallServer();
      ~SyscallServer();

      void handleSleepCall(thread_id_t thread_id, SubsecondTime wake_time, SubsecondTime curr_time, SubsecondTime &end_time);
      IntPtr handleFutexCall(thread_id_t thread_id, futex_args_t &args, SubsecondTime curr_time, SubsecondTime &end_time);
      SubsecondTime getNextTimeout(SubsecondTime time);

   private:
      // Handling Futexes
      IntPtr futexWait(thread_id_t thread_id, int *uaddr, int val, int act_val, int val3, SubsecondTime curr_time, SubsecondTime timeout_time, SubsecondTime &end_time);
      IntPtr futexWake(thread_id_t thread_id, int *uaddr, int nr_wake, int val3, SubsecondTime curr_time, SubsecondTime &end_time);
      IntPtr futexWakeOp(thread_id_t thread_id, int *uaddr, int *uaddr2, int nr_wake, int nr_wake2, int op, SubsecondTime curr_time, SubsecondTime &end_time);
      IntPtr futexCmpRequeue(thread_id_t thread_id, int *uaddr, int val, int *uaddr2, int val3, int act_val, SubsecondTime curr_time, SubsecondTime &end_time);

      SimFutex* findFutexByUaddr(int *uaddr, thread_id_t thread_id);
      thread_id_t wakeFutexOne(SimFutex *sim_futex, thread_id_t thread_by, int mask, SubsecondTime curr_time);
      int futexDoOp(Core *core, int op, int *uaddr);

      void futexPeriodic(SubsecondTime time);

      SubsecondTime applyRescheduleCost(thread_id_t thread_id, bool conditional = true);

      static SInt64 hook_periodic(UInt64 ptr, UInt64 time)
      {
         ((SyscallServer*)ptr)->futexPeriodic(*(subsecond_time_t*)(&time));
         return 0;
      }

      SubsecondTime m_reschedule_cost;

      // Sleeping threads
      SimFutex::ThreadQueue m_sleeping;

      // Handling Futexes
      typedef std::unordered_map<IntPtr, SimFutex> FutexMap;
      FutexMap m_futexes;

      friend class ThreadManager;
};

#endif
