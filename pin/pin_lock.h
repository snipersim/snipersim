#pragma once

#include "lock.h"
#include "pin.H"

class PinLock : public LockImplementation
{
public:
   PinLock();
   ~PinLock();

   void acquire();
   void release();

private:
   PIN_LOCK _lock;
};
