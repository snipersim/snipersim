#ifndef __PIN_EXCEPTIONS_H
#define __PIN_EXCEPTIONS_H

#include "fixed_types.h"

#include "pin.H"

EXCEPT_HANDLING_RESULT exceptionHandler(THREADID tid, EXCEPTION_INFO *pExceptInfo, PHYSICAL_CONTEXT *pPhysCtxt, VOID *v);

#endif // __PIN_EXCEPTIONS_H
