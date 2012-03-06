#ifndef ITOSTR_H
#define ITOSTR_H

#include "fixed_types.h"

#include <sstream>

// Don't use std::string externally since it's not multithread safe
// But the one internal to std::stringstream is never exposed to other threads,
// so it should be fine to use it locally.

template<typename T>
String itostr(T val)
{
   std::stringstream s;
   s << val;
   return String(s.str().c_str());
}

#endif // ITOSTR_H
