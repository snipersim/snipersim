#include "pin_lock.h"

PinLock::PinLock()
{
   PIN_InitLock(&_lock);
}

PinLock::~PinLock()
{
}

void PinLock::acquire()
{
   PIN_GetLock(&_lock, 1);
}

void PinLock::release()
{
   PIN_ReleaseLock(&_lock);
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
