#include "selock.h"
#include <assert.h>

SELock::SELock(void)
   : m_write(false)
   , m_writers(0)
   , m_readers(0)
{
   #ifdef TIME_LOCKS
   _timer = TotalTimer::getTimerByStacktrace("selock@" + itostr(this));
   #endif
}

#define WAIT_WHILE(condition)                      \
   /* First busy wait a little */                  \
   for(int i = 0; i < 10000 && (condition); ++i) ; \
   while(condition) {                              \
      /* Then reschedule */                        \
      sched_yield();                               \
   }

// Acquire exclusive access
void
SELock::acquire_exclusive(void)
{
   #ifdef TIME_LOCKS
   ScopedTimer tt(*_timer);
   #endif

   // Tell everyone we want to write
   __sync_add_and_fetch(&m_writers, 1);

   while(true) {
      // Wait until the current writer and all readers have left
      WAIT_WHILE(m_write || m_readers);

      // No readers can come in, but another writer may have beaten us
      if (__sync_lock_test_and_set(&m_write, 1) == 1) {
         // Yep, someone was there already, retry
         continue;
      }

      // m_write was already set by the test_and_set above
      // Remove ourselves from the waiting writers count
      __sync_sub_and_fetch(&m_writers, 1);

      break;
   }
}

// Release exclusive access
void
SELock::release_exclusive(void)
{
   assert(m_write == true);

   m_write = 0;
}

// Acquire shared access
void
SELock::acquire_shared(void)
{
   #ifdef TIME_LOCKS
   ScopedTimer tt(*_timer);
   #endif

   while(true) {
      // Wait until the current writer has left
      WAIT_WHILE(m_write);

      // Now increment m_readers, but we need to be sure no writer comes in inbetween
      // Thus, make ourselves writer for a bit
      // Note that we did not wait for m_readers == 0, so there is no(t much) extra delay

      if (__sync_lock_test_and_set(&m_write, 1) == 1) {
         // A writer already came in, retry
         continue;
      }

      // Increment the readers count
      __sync_add_and_fetch(&m_readers, 1);

      m_write = 0;
      break;
   }
}

// Release shared access
void
SELock::release_shared(void)
{
   assert(m_readers > 0);

   __sync_sub_and_fetch(&m_readers, 1);
}

void
SELock::upgrade(void)
{
   assert(m_readers > 0);

   // Tell everyone we want to write
   UInt64 prev_writers = __sync_add_and_fetch(&m_writers, 1);

   if (prev_writers) {
      // Someone is waiting to write. Release our read lock, and go the long way (same as acquire_exclusive)
      __sync_sub_and_fetch(&m_readers, 1);

      while(true) {
         // Wait until the current writer and all readers have left
         WAIT_WHILE(m_write || m_readers);

         // No readers can come in, but another writer may have beaten us
         if (__sync_lock_test_and_set(&m_write, 1) == 1) {
            // Yep, someone was there already, retry
            continue;
         }

         // m_write was already set by the test_and_set above
         // Remove ourselves from the waiting writers count
         __sync_sub_and_fetch(&m_writers, 1);

         break;
      }

   } else {
      // There were no previous waiting writers. We don't need to release the shared lock we already have

      // Wait until the current writer and all readers have left
      WAIT_WHILE(m_write || m_readers);

      // No readers can come in, nor any other writer since we kept our readers count
      m_write = 1;

      // Now remove ourselves from the readers
      __sync_sub_and_fetch(&m_readers, 1);

      // m_write was already set by the test_and_set above
      // Remove ourselves from the waiting writers count
      __sync_sub_and_fetch(&m_writers, 1);
   }
}

void
SELock::downgrade(void)
{
   assert(m_write == true);

   // First make ourselves a reader, so no other potential writer may come in yet (until we fully release)
   __sync_add_and_fetch(&m_readers, 1);

   m_write = 0;
}
