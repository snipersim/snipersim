#ifndef __PINBOOST_DEBUG_H
#define __PINBOOST_DEBUG_H

#include "pinboost_assert.h"

#include "pin.H"

extern bool pinboost_do_debug;

void pinboost_register(const char* name, bool do_screen_debug);

bool pinboost_backtrace(EXCEPTION_INFO *pExceptInfo, PHYSICAL_CONTEXT *pPhysCtxt);
void pinboost_debugme(THREADID threadid);

#endif // __PINBOOST_DEBUG_H
