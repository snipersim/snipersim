#ifndef COND_H
#define COND_H

#include "fixed_types.h"
#include "lock.h"

// Our condition variable interface is slightly different from
// pthreads in that the mutex associated with the condition variable
// is built into the condition variable itself.

class ConditionVariable
{
   public:
      ConditionVariable();
      ~ConditionVariable();

      // must acquire lock before entering wait. will own lock upon exit.
      void wait(Lock& _lock, UInt64 timeout_ns = 0);
      void signal();
      void broadcast();

   private:
      int m_futx;
      Lock m_lock;
      #ifdef TIME_LOCKS
      TotalTimer* _timer;
      #endif
};

#endif // COND_H
