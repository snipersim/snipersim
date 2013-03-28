#ifndef CIRCULAR_QUEUE_H
#define CIRCULAR_QUEUE_H

#include <assert.h>
#include <string.h>

template <class T> class CircularQueue
{
   private:
      const UInt32 m_size;
      volatile UInt32 m_first; // next element to be inserted here
      UInt8 padding1[60];
      volatile UInt32 m_last;  // last element is here
      UInt8 padding2[60];
      T* const m_queue;

   public:
      typedef T value_type;
      class iterator : public std::iterator<std::forward_iterator_tag, T, std::ptrdiff_t, const T*, const T&>
      {
         private:
            CircularQueue &_queue;
            UInt32 _idx;
         public:
            iterator(CircularQueue &queue, UInt32 idx) : _queue(queue), _idx(idx) {}
            T& operator*() const { return _queue.at(_idx); }
            T* operator->() const { return &_queue.at(_idx); }
            iterator& operator++() { _idx++; return *this; }
            bool operator==(iterator const& rhs) const { return &_queue == &rhs._queue && _idx == rhs._idx; }
            bool operator!=(iterator const& rhs) const { return ! (*this == rhs); }
      };

      CircularQueue(UInt32 size = 63);
      CircularQueue(const CircularQueue &queue);
      ~CircularQueue();
      void push(const T& t);
      void pushCircular(const T& t);
      T& next(void);
      T pop(void);
      T& front(void);
      const T& front(void) const;
      T& back(void);
      const T& back(void) const;
      bool full(void) const;
      bool empty(void) const;
      UInt32 size(void) const;
      iterator begin(void) { return iterator(*this, 0); }
      iterator end(void) { return iterator(*this, size()); }
      T& operator[](UInt32 idx) const { return m_queue[(m_last + idx) % m_size]; }
      T& at(UInt32 idx) const { assert(idx < size()); return (*this)[idx]; }
};

template <class T>
CircularQueue<T>::CircularQueue(UInt32 size)
   // Since we use head == tail as the empty condition instead of an extra empty flag, we can hold at most m_size-1 elements
   : m_size(size + 1)
   , m_first(0)
   , m_last(0)
   , m_queue(new T[m_size])
{
}

// Copy only the size parameter from the other CircularQueue
template <class T>
CircularQueue<T>::CircularQueue(const CircularQueue &queue)
   : m_size(queue.m_size)
   , m_first(0)
   , m_last(0)
   , m_queue(new T[m_size])
{
   assert(queue.size() == 0);
}

template <class T>
CircularQueue<T>::~CircularQueue()
{
   delete [] m_queue;
}

template <class T>
void
CircularQueue<T>::push(const T& t)
{
   assert(!full());
   m_queue[m_first] = t;
   m_first = (m_first + 1) % m_size;
}

template <class T>
void
CircularQueue<T>::pushCircular(const T& t)
{
  if (full())
    pop();
  push(t);
}

template <class T>
T&
CircularQueue<T>::next(void)
{
   assert(!full());
   T& t = m_queue[m_first];
   m_first = (m_first + 1) % m_size;
   return t;
}

template <class T>
T
CircularQueue<T>::pop()
{
   assert(!empty());
   UInt32 idx = m_last;
   m_last = (m_last + 1) % m_size;
   return m_queue[idx];
}

template <class T>
T &
CircularQueue<T>::front()
{
   assert(!empty());
   return m_queue[m_last];
}

template <class T>
const T &
CircularQueue<T>::front() const
{
   assert(!empty());
   return m_queue[m_last];
}

template <class T>
T &
CircularQueue<T>::back()
{
   assert(!empty());
   return m_queue[(m_first + m_size - 1) % m_size];
}

template <class T>
const T &
CircularQueue<T>::back() const
{
   assert(!empty());
   return m_queue[(m_first + m_size - 1) % m_size];
}

template <class T>
bool
CircularQueue<T>::full(void) const
{
   return (m_first + 1) % m_size == m_last;
}

template <class T>
bool
CircularQueue<T>::empty(void) const
{
   return m_first == m_last;
}

template <class T>
UInt32
CircularQueue<T>::size() const
{
   return (m_first + m_size - m_last) % m_size;
}


#endif // CIRCULAR_QUEUE_H
