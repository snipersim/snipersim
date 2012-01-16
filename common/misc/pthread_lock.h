#ifndef PTHREAD_LOCK_H
#define PTHREAD_LOCK_H

#include "lock.h"

#include <pthread.h>

class PthreadLock : public LockImplementation
{
public:
   PthreadLock();
   ~PthreadLock();

   void acquire();
   void release();

private:
   pthread_mutex_t _mutx;
};

#endif // PTHREAD_LOCK_H
