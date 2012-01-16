#ifndef MT_CIRCULAR_QUEUE_H
#define MT_CIRCULAR_QUEUE_H

#include "circular_queue.h"
#include "lock.h"
#include "cond.h"

template <class T> class MTCircularQueue : public CircularQueue<T>
{
   private:
      Lock m_lock;
      ConditionVariable m_full;
      ConditionVariable m_empty;
   public:
      class iterator {
          public:
             // NOTE: unless we add locking, iterating over a MTCircularQueue which can be updated by another thread is unsafe!
             iterator(const MTCircularQueue &queue, UInt32 idx) = 0;
      };
      MTCircularQueue(UInt32 size = 64) : CircularQueue<T>(size) {}
      void push(const T& t);
      void push_wait(const T& t);
      void push_locked(const T& t);
      T pop(void);
      T pop_wait(void);
      T pop_locked(void);
      void full_wait(void);
      void empty_wait(void);
      void full_wait_locked(void);
      void empty_wait_locked(void);
};

template <class T>
void
MTCircularQueue<T>::full_wait(void)
{
   ScopedLock sl(m_lock);
   full_wait_locked();
}

template <class T>
void
MTCircularQueue<T>::full_wait_locked(void)
{
   while(CircularQueue<T>::full())
      m_full.wait(m_lock);
}

template <class T>
void
MTCircularQueue<T>::empty_wait(void)
{
   ScopedLock sl(m_lock);
   empty_wait_locked();
}

template <class T>
void
MTCircularQueue<T>::empty_wait_locked()
{
   while(CircularQueue<T>::empty())
      m_empty.wait(m_lock);
}



template <class T>
void
MTCircularQueue<T>::push_locked(const T& t)
{
   bool wasEmpty = CircularQueue<T>::empty();

   CircularQueue<T>::push(t);

   if (wasEmpty)
      m_empty.signal();
}

template <class T>
void
MTCircularQueue<T>::push(const T& t)
{
   ScopedLock sl(m_lock);
   push_locked(t);
}

template <class T>
void
MTCircularQueue<T>::push_wait(const T& t)
{
   ScopedLock sl(m_lock);
   full_wait_locked();
   push_locked(t);
}



template <class T>
T
MTCircularQueue<T>::pop_locked()
{
   bool wasFull = CircularQueue<T>::full();

   T t = CircularQueue<T>::pop();

   if (wasFull)
      m_full.signal();

   return t;
}

template <class T>
T
MTCircularQueue<T>::pop()
{
   ScopedLock sl(m_lock);
   return pop_locked();
}

template <class T>
T
MTCircularQueue<T>::pop_wait()
{
   ScopedLock sl(m_lock);
   empty_wait_locked();
   return pop_locked();
}

#endif //MT_CIRCULAR_QUEUE_H
