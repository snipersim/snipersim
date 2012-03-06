#ifndef SETLOCK_H
#define SETLOCK_H

#include "lock.h"
#include "selock.h"

#include <vector>
#include <pthread.h>

/* Cache set lock */

class _SetLock
{
   public:
      _SetLock(UInt32 core_offset, UInt32 num_sharers);
      void acquire_exclusive(void);
      void release_exclusive(void);
      void acquire_shared(UInt32 core_id);
      void release_shared(UInt32 core_id);
      void upgrade(UInt32 core_id);
      void downgrade(UInt32 core_id);

   private:
      class PersetLock
      {
         public:
            PersetLock() { pthread_mutex_init(&_mutx, NULL); }
            void acquire() { pthread_mutex_lock(&_mutx); }
            void release() { pthread_mutex_unlock(&_mutx); }
         private:
            pthread_mutex_t _mutx;
      } __attribute__ ((aligned (64)));

      std::vector<PersetLock> m_locks;
      UInt32 m_core_offset;
      #ifdef TIME_LOCKS
      TotalTimer* _timer;
      #endif
};

class _SELock : SELock
{
   public:
      _SELock(UInt32 core_offset, UInt32 num_sharers) : SELock() {}
      void acquire_shared(UInt32 core_id) { SELock::acquire_shared(); }
      void release_shared(UInt32 core_id) { SELock::release_shared(); }
      void downgrade(UInt32 core_id)      { SELock::downgrade(); }
      void upgrade(UInt32 core_id)        { SELock::upgrade(); }
};

#if 0
  typedef SELock SetLock;
#else
  typedef _SetLock SetLock;
#endif

#endif // SETLOCK_H
