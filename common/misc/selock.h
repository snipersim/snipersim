#ifndef SELOCK_H
#define SELOCK_H

#include "lock.h"

/* Shared/Exclusive (Reader/Writer) lock implementation */

class SELock
{
   public:
      SELock(void);
      void acquire_exclusive(void);
      void release_exclusive(void);
      void acquire_shared(void);
      void release_shared(void);
      void upgrade(void);
      void downgrade(void);

   private:
      Lock m_lock;
      bool m_write;     // Currently locked for writing
      UInt64 m_writers; // Number of waiting writers
      UInt64 m_readers; // Number of current readers
      #ifdef TIME_LOCKS
      TotalTimer* _timer;
      #endif
};

#endif // SELOCK_H
