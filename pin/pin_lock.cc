#include "pin_lock.h"

PinLock::PinLock()
{
   InitLock(&_lock);
}

PinLock::~PinLock()
{
}

void PinLock::acquire()
{
   GetLock(&_lock, 1);
}

void PinLock::release()
{
   ReleaseLock(&_lock);
}

#if 1
LockImplementation* LockCreator_Default::create()
{
   return new PinLock();
}
LockImplementation* LockCreator_RwLock::create()
{
   return new PinLock();
}
#endif

LockImplementation* LockCreator_Spinlock::create()
{
    return new PinLock();
}
