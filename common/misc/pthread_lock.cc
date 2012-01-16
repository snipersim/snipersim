#include "pthread_lock.h"

PthreadLock::PthreadLock()
{
   pthread_mutex_init(&_mutx, NULL);
}

PthreadLock::~PthreadLock()
{
   pthread_mutex_destroy(&_mutx);
}

void PthreadLock::acquire()
{
   pthread_mutex_lock(&_mutx);
}

void PthreadLock::release()
{
   pthread_mutex_unlock(&_mutx);
}

__attribute__((weak)) LockImplementation* LockCreator_Default::create()
{
    return new PthreadLock();
}

__attribute__((weak)) LockImplementation* LockCreator_RwLock::create()
{
    return new PthreadLock();
}

__attribute__((weak)) LockImplementation* LockCreator_Spinlock::create()
{
    return new PthreadLock();
}
