#ifndef PROGRESS_TRACE_H
#define PROGRESS_TRACE_H

#include "pin.H"

VOID initProgressTrace();
VOID shutdownProgressTrace();
VOID threadStartProgressTrace();
VOID addProgressTrace(INS ins);

#endif
