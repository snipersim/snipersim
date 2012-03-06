#ifndef LOCK_H
#define LOCK_H

//#define TIME_LOCKS

#include "itostr.h"

#ifdef TIME_LOCKS
#include "timer.h"
#endif


/* Lock Implementation and Creator class (allows ::create to be overridden from inside /pin) */

class LockImplementation
{
public:
   LockImplementation() {}
   virtual ~LockImplementation() {}

   virtual void acquire() = 0;
   virtual void release() = 0;
   virtual void acquire_read() { acquire(); }
   virtual void release_read() { release(); }
};

class LockCreator
{
   public:
      static LockImplementation* create();
};


/* Actual Lock class to use in other objects */

class BaseLock
{
   public:
      virtual void acquire() = 0;
      virtual void release() = 0;
      virtual void acquire_read() = 0;
      virtual void release_read() = 0;
};

template <class T_LockCreator> class TLock : public BaseLock
{
public:
   TLock()
   {
      _lock = T_LockCreator::create();
      #ifdef TIME_LOCKS
      _timer = TotalTimer::getTimerByStacktrace("lock@" + itostr(this));
      #endif
   }

   ~TLock()
   {
      delete _lock;
      #ifdef TIME_LOCKS
      delete _timer;
      #endif
   }

   void acquire()
   {
      #ifdef TIME_LOCKS
      ScopedTimer tt(*_timer);
      #endif
      _lock->acquire();
   }

   void acquire_read()
   {
      #ifdef TIME_LOCKS
      ScopedTimer tt(*_timer);
      #endif
      _lock->acquire_read();
   }

   void release()
   {
      _lock->release();
   }

   void release_read()
   {
      _lock->release_read();
   }

private:
   LockImplementation* _lock;
   #ifdef TIME_LOCKS
   TotalTimer* _timer;
   #endif
};


class LockCreator_Default : public LockCreator
{
   public:
      static LockImplementation* create();
};
class LockCreator_RwLock : public LockCreator
{
   public:
      static LockImplementation* create();
};
class LockCreator_Spinlock : public LockCreator
{
   public:
      static LockImplementation* create();
};
class LockCreator_NullLock : public LockCreator
{
   public:
      static LockImplementation* create();
};

typedef TLock<LockCreator_Default> Lock;
typedef TLock<LockCreator_RwLock> RwLock;
typedef TLock<LockCreator_Spinlock> SpinLock;
typedef TLock<LockCreator_NullLock> NullLock;


/* Helper class: hold a lock for the scope of this object */

class ScopedLock
{
   BaseLock &_lock;

public:
   ScopedLock(BaseLock &lock)
      : _lock(lock)
   {
      _lock.acquire();
   }

   ~ScopedLock()
   {
      _lock.release();
   }
};


class ScopedReadLock
{
   BaseLock &_lock;

public:
   ScopedReadLock(BaseLock &lock)
      : _lock(lock)
   {
      _lock.acquire_read();
   }

   ~ScopedReadLock()
   {
      _lock.release_read();
   }
};


#endif // LOCK_H
