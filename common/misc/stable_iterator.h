#ifndef STABLE_ITERATOR_H
#define STABLE_ITERATOR_H

#include <vector>

template
<class T> class StableIterator
{
   public:
      StableIterator(const StableIterator<T> &s)
            : _vec(s._vec), _offset(s._offset) {}

      StableIterator(std::vector<T> &vec, unsigned int offset)
            : _vec(vec), _offset(offset) {}
      T* getPtr() { return &(_vec[_offset]); }
      T* operator->() { return getPtr(); }
      T& operator*() { return *getPtr(); }

      StableIterator<T> operator=(const StableIterator<T>&src)
      {
         return StableIterator<T>(src._vec, src._offset);
      }
   private:
      std::vector<T> & _vec;
      unsigned int _offset;
};

#endif
