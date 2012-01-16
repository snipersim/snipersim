#ifndef __AVERAGE_H
#define __AVERAGE_H

template <class T>
typename T::value_type
arithmetic_mean(T& list)
{
   typename T::value_type sum = typename T::value_type();
   size_t count = 0;

   for(typename T::iterator pos = list.begin(); pos != list.end(); ++pos) {
      sum += *pos;
      ++count;
   }
   return sum / count;
}

#endif // __AVERAGE_H
