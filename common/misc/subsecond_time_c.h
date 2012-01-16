#ifndef __SUBSECOND_TIME_C_H
#define __SUBSECOND_TIME_C_H

// POD and c-linkage subsecond-time header

#ifdef __cplusplus
#include <iostream>
#include <cstdint>
#else
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
// This subsecond_time_t struct is used for c-linkage cases
struct subsecond_time_s {
#ifdef __cplusplus
   // Remove all constructors to make this data structure POD
   // Conversion to/from SubsecondTime is defined by the SubsecondTime class
   // From http://www.stackoverflow.com/questions/4421706
   subsecond_time_s& operator+=(const subsecond_time_s &rhs)
   {
      m_time += rhs.m_time;
      return *this;
   }
#endif
   uint64_t m_time;
};
typedef struct subsecond_time_s subsecond_time_t;
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
std::ostream &operator<<(std::ostream &os, const subsecond_time_t &time);
#endif

#endif /* __SUBSECOND_TIME_C_H */

