#ifndef __CLOCK_SKEW_MINIMIZATION_H__
#define __CLOCK_SKEW_MINIMIZATION_H__

#include "fixed_types.h"
#include "pin.H"

void handlePeriodicSync(THREADID thread_id);
void addPeriodicSync(TRACE trace, INS ins);

#endif /* __CLOCK_SKEW_MINIMIZATION_H__ */
