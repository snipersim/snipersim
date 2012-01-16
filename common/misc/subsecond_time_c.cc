#include "subsecond_time.h"

// Cannot make this inline because of the SubsecondTime dependency
std::ostream &operator<<(std::ostream &os, const subsecond_time_t &time)
{
   return (os << static_cast<SubsecondTime>(time));
}
