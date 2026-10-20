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
   // glibc records the owner's thread id in every mutex kind
   long ownerTid() const { return _mutx.__data.__owner; }

private:
   pthread_mutex_t _mutx;
};

#endif // PTHREAD_LOCK_H
